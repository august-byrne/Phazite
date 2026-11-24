// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation's writers on its values (roadmap step S5.20, modulation's third piece; model/modulation/internal/Modulation.h):
// every edit of a modulator or a macro works its next value out and sets it through modulation's setters (setModulator(),
// eraseModulator(), setModulatorOrder(), setAllModulation()) into modulation's area. Each edit leaves the song, written whole
// (WholeState.h), holding the element Phazite's writers always left (built here as they built it, on an element of the test's
// own; its VALUES before its MODULATES, as whole state writes it), as one exact undo step; a set as the song holds it writes
// nothing. And what copies, forgets or applies
// modulation does it through them: a duplicated track (its macros' automation with them, its containers' too) and chain, a
// bounce's copies, a folder made a bus and back, the versions' pass (a version applied each way, a track's line, a folder's
// line that brings its tracks back, the master's line, the song as copied, a song opened from a file whose modulators the
// loader made as the song holds them) and a merge's choice of each kind of clash; and a modulator or a macro removed takes
// its MIDI links with it (known bug 241).
//   phazite_tests.exe --source-file=*ModulationWritersTests*
#include "SongCheck.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Json.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongMerge.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::DeviceID;
using phazite::ModulatorID;
using phazite::ModulatorPlace;
using phazite::ModulatorType;
using phazite::ModulatorValue;
using phazite::ParameterRef;
using phazite::TrackID;

ParameterRef parameterOf (ModulatorID modulator, const juce::String& id)
{
    return { DeviceID::fromRaw (modulator.getRaw()), id };
}

ParameterRef valueOf (ModulatorID macro)
{
    return parameterOf (macro, juce::String (macro.getRaw())); // a macro's one parameter has its own ID
}

const phazite::devices::ParameterInfo& infoOf (phazite::devices::ModulatorKind kind, const char* parameter)
{
    const auto index = phazite::devices::findModulatorParameter (kind, parameter);
    REQUIRE (index >= 0);
    return phazite::devices::getModulatorParameters (kind)[(size_t) index];
}

// What modulation's writers wrote before they worked on values (model/modulation/Modulation.cpp and
// model/devices/Parameters.cpp up to roadmap step S5.20's third piece), on elements of the test's own: the tree each edit
// must still leave.
namespace asBefore
{
    void writeSteps (juce::ValueTree modulator, std::vector<float> steps)
    {
        while (! steps.empty() && std::abs (steps.back()) < 1.0e-4f)
            steps.pop_back();

        juce::StringArray values;

        for (const auto step : steps)
            values.add (juce::String (std::round ((double) step * 1.0e4) / 1.0e4));

        if (values.isEmpty())
            modulator.removeProperty (schema::modulatorSteps, nullptr);
        else
            modulator.setProperty (schema::modulatorSteps, values.joinIntoString (" "), nullptr);
    }

    juce::ValueTree addModulator (ModulatorID id, ModulatorType type)
    {
        juce::ValueTree modulator (schema::MODULATOR);
        modulator.setProperty (schema::modulatorID, (juce::int64) id.getRaw(), nullptr);
        modulator.setProperty (schema::modulatorType, schema::preset::modulatorTypeNames[(size_t) type], nullptr);
        const auto kind = (phazite::devices::ModulatorKind) type;

        if (type == ModulatorType::lfo || type == ModulatorType::random)
            phazite::test::writeValueSetByHand (modulator, infoOf (kind, "bipolar"), 1.0f);

        if (type == ModulatorType::steps)
        {
            const auto numSteps = juce::roundToInt (infoOf (kind, "numSteps").defaultValue);
            std::vector<float> steps;

            for (int i = 0; i < numSteps; ++i)
                steps.push_back (numSteps > 1 ? -1.0f + 2.0f * (float) i / (float) (numSteps - 1) : 0.0f);

            writeSteps (modulator, steps);
        }

        return modulator;
    }

    juce::ValueTree addMacro (ModulatorID id, const juce::String& name)
    {
        juce::ValueTree macro (schema::MACRO);
        macro.setProperty (schema::modulatorID, (juce::int64) id.getRaw(), nullptr);
        macro.setProperty (schema::macroName, name, nullptr);
        return macro;
    }

    void setValue (juce::ValueTree modulator, phazite::devices::ModulatorKind kind, const char* parameter, float value)
    {
        const auto& info = infoOf (kind, parameter);
        phazite::test::writeValueSetByHand (modulator, info, phazite::devices::constrain (info, value));
    }

    void setMacroValue (juce::ValueTree macro, float value)
    {
        if (juce::exactlyEqual (value, 0.0f))
            macro.removeProperty (schema::macroValue, nullptr);
        else
            macro.setProperty (schema::macroValue, value, nullptr);
    }

    void setOn (juce::ValueTree modulator, bool on)
    {
        if (on)
            modulator.removeProperty (schema::deviceOff, nullptr);
        else
            modulator.setProperty (schema::deviceOff, true, nullptr);
    }

    void setStep (juce::ValueTree modulator, int index, float value)
    {
        std::vector<float> steps;

        for (const auto& step : juce::StringArray::fromTokens (modulator[schema::modulatorSteps].toString(), " ", {}))
            steps.push_back (juce::jlimit (-1.0f, 1.0f, phazite::readFloat (step)));

        steps.resize (std::max (steps.size(), (size_t) index + 1), 0.0f);
        steps[(size_t) index] = juce::jlimit (-1.0f, 1.0f, value);
        writeSteps (modulator, steps);
    }

    juce::ValueTree findModulates (const juce::ValueTree& modulator, const ParameterRef& parameter)
    {
        for (const auto& child : modulator)
            if (child.hasType (schema::MODULATES) && phazite::readID (child[schema::modulatesDevice]) == parameter.device.getRaw()
                && child[schema::modulatesParameter].toString() == parameter.parameter)
                return child;

        return {};
    }

    void setModulation (juce::ValueTree modulator, const ParameterRef& parameter, float amount)
    {
        auto modulates = findModulates (modulator, parameter);

        if (! modulates.isValid())
        {
            modulates = juce::ValueTree (schema::MODULATES);
            modulates.setProperty (schema::modulatesDevice, juce::String ((juce::int64) parameter.device.getRaw()), nullptr);
            modulates.setProperty (schema::modulatesParameter, parameter.parameter, nullptr);
            modulator.appendChild (modulates, nullptr);
        }

        modulates.setProperty (schema::modulatesAmount, std::round ((double) juce::jlimit (-1.0f, 1.0f, amount) * 1.0e4) / 1.0e4, nullptr);
    }

    void removeModulation (juce::ValueTree modulator, const ParameterRef& parameter)
    {
        modulator.removeChild (findModulates (modulator, parameter), nullptr);
    }
} // namespace asBefore

// A modulator's or a macro's element in a whole copy of the song (phazite::test::copyWholeSong(): the song's tree holds none),
// found by its ID; an invalid tree for none. Its parent is the copy's while the copy lives.
juce::ValueTree findModulatorIn (const juce::ValueTree& whole, ModulatorID id)
{
    for (const auto& child : whole)
    {
        if ((child.hasType (schema::MODULATOR) || child.hasType (schema::MACRO))
            && phazite::readID (child[schema::modulatorID]) == id.getRaw())
            return child;

        if (const auto found = findModulatorIn (child, id); found.isValid())
            return found;
    }

    return {};
}

// A modulator's or a macro's element in the song whole as text, and whether it is the last of its owner's children there
// (where every edit adds one, its owner holding no send).
juce::String written (const phazite::Song& song, ModulatorID id)
{
    return findModulatorIn (phazite::test::copyWholeSong (song), id).toXmlString();
}

bool isLastChild (const phazite::Song& song, ModulatorID id)
{
    const auto whole = phazite::test::copyWholeSong (song);
    const auto element = findModulatorIn (whole, id);
    return element.isValid() && element.getParent().indexOf (element) == element.getParent().getNumChildren() - 1;
}

// An element of the test's own as the song writes it whole: a modulator's VALUES before its MODULATES (ModulationValue.h:
// writeModulatorElement(); the old writers appended a VALUES made after a modulation after it, which the loader moves before
// it as the song opens).
juce::ValueTree asWhole (const juce::ValueTree& element)
{
    auto copy = element.createCopy();

    if (auto values = copy.getChildWithName (schema::VALUES); values.isValid())
    {
        copy.moveChild (copy.indexOf (values), 0, nullptr);

        // Its values in its kind's parameters' order, as whole state writes them (the old writers in the order first set).
        if (const auto kind = phazite::readModulatorKind (copy))
        {
            juce::ValueTree ordered (schema::VALUES);

            for (const auto& parameter : phazite::devices::getModulatorParameters (*kind))
                if (const juce::Identifier name (juce::String (parameter.id.data(), parameter.id.size())); values.hasProperty (name))
                    ordered.setProperty (name, values[name], nullptr);

            values.removeAllProperties (nullptr); // so that each is set again, in this order
            values.copyPropertiesFrom (ordered, nullptr);
        }
    }

    return copy;
}

// A step of the test's own, around what isn't a change of its own (a setter, a pass, a choice).
std::function<void()> inStep (phazite::Song& song, const juce::String& name, std::function<void()> change)
{
    return [&song, name, change = std::move (change)]
    {
        const phazite::UndoStep step (song, name);
        change();
    };
}

// The IDs of an owner's modulators and macros in the song, in their order.
std::vector<ModulatorID> idsOf (const phazite::Song& song, const ModulatorPlace& owner)
{
    std::vector<ModulatorID> ids;

    for (const auto& each : phazite::getOwnedModulators (song, owner))
        ids.push_back (each.id);

    return ids;
}
} // namespace

TEST_CASE ("Modulation's writers: each edit leaves the element as Phazite's writers always left it, one exact undo step")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto volume = s.getVolumeParameter (track), pan = s.getPanParameter (track);
    phazite::test::UndoCheck undo (context);
    ModulatorID lfo, steps, macro;

    undo.check ("Add LFO", [&] { lfo = phazite::addModulator (context, track, ModulatorType::lfo); });
    REQUIRE (lfo.isValid());
    auto lfoBefore = asBefore::addModulator (lfo, ModulatorType::lfo);
    CHECK (written (song, lfo) == asWhole (lfoBefore).toXmlString());
    CHECK (isLastChild (song, lfo));

    undo.check ("Add Step Sequencer", [&] { steps = phazite::addModulator (context, track, ModulatorType::steps); });
    REQUIRE (steps.isValid());
    auto stepsBefore = asBefore::addModulator (steps, ModulatorType::steps);
    CHECK (written (song, steps) == asWhole (stepsBefore).toXmlString());
    CHECK (isLastChild (song, steps));

    undo.check ("Add Macro", [&] { macro = phazite::addModulator (context, track, ModulatorType::macro); });
    REQUIRE (macro.isValid());
    auto macroBefore = asBefore::addMacro (macro, "Macro 1");
    CHECK (written (song, macro) == asWhole (macroBefore).toXmlString());
    CHECK (isLastChild (song, macro));

    undo.check ("Rename Macro", [&] { phazite::renameMacro (context, macro, "  Air "); });
    macroBefore.setProperty (schema::macroName, "Air", nullptr);
    CHECK (written (song, macro) == asWhole (macroBefore).toXmlString());

    // A macro turned, and back to 0: its value only while not 0.
    undo.check ("Change Macro", [&] { s.setMacroValue (macro, 0.75f); });
    asBefore::setMacroValue (macroBefore, 0.75f);
    CHECK (written (song, macro) == asWhole (macroBefore).toXmlString());
    undo.check ("Change Macro Back", [&] { s.setMacroValue (macro, 0.0f); });
    asBefore::setMacroValue (macroBefore, 0.0f);
    CHECK (written (song, macro) == asWhole (macroBefore).toXmlString());

    // A modulator's values set by hand: one its VALUES has, then one of a modulator that modulates something already (its
    // VALUES written before what it modulates, as the song writes it whole), then that one set back to its default (its VALUES
    // go with it).
    undo.check ("Change Rate", [&] { s.setModulatorParameter (lfo, "rate", 2.0f); });
    asBefore::setValue (lfoBefore, phazite::devices::ModulatorKind::lfo, "rate", 2.0f);
    CHECK (written (song, lfo) == asWhole (lfoBefore).toXmlString());

    undo.check ("Modulate Pan", [&] { phazite::modulate (context, steps, pan, 0.25f); });
    asBefore::setModulation (stepsBefore, pan, 0.25f);
    CHECK (written (song, steps) == asWhole (stepsBefore).toXmlString());
    undo.check ("Change Depth", [&] { s.setModulatorParameter (steps, "depth", 0.5f); });
    asBefore::setValue (stepsBefore, phazite::devices::ModulatorKind::steps, "depth", 0.5f);
    CHECK (written (song, steps) == asWhole (stepsBefore).toXmlString());
    {
        const auto whole = phazite::test::copyWholeSong (song);
        const auto element = findModulatorIn (whole, steps);
        REQUIRE (element.getNumChildren() == 2);
        CHECK (element.getChild (0).hasType (schema::VALUES));
    }
    undo.check ("Change Depth Back", [&] { s.setModulatorParameter (steps, "depth", 1.0f); });
    asBefore::setValue (stepsBefore, phazite::devices::ModulatorKind::steps, "depth", 1.0f);
    CHECK (written (song, steps) == asWhole (stepsBefore).toXmlString());
    CHECK_FALSE (findModulatorIn (phazite::test::copyWholeSong (song), steps).getChildWithName (schema::VALUES).isValid());

    // A step, one past the steps it has, and one set to 0 at the end.
    undo.check ("Change Step", [&] { phazite::setStep (context, steps, 3, 0.25f); });
    asBefore::setStep (stepsBefore, 3, 0.25f);
    CHECK (written (song, steps) == asWhole (stepsBefore).toXmlString());
    undo.check ("Change Step", inStep (song, "Change Step", [&] { phazite::setModulatorStep (song, steps, 20, -0.5f); }));
    asBefore::setStep (stepsBefore, 20, -0.5f);
    CHECK (written (song, steps) == asWhole (stepsBefore).toXmlString());
    undo.check ("Change Step", inStep (song, "Change Step", [&] { phazite::setModulatorStep (song, steps, 20, 0.0f); }));
    asBefore::setStep (stepsBefore, 20, 0.0f);
    CHECK (written (song, steps) == asWhole (stepsBefore).toXmlString());

    // Switched off and on: its switch only while off.
    undo.check ("Turn Modulator Off", [&] { phazite::setModulatorOn (context, lfo, false); });
    asBefore::setOn (lfoBefore, false);
    CHECK (written (song, lfo) == asWhole (lfoBefore).toXmlString());
    undo.check ("Turn Modulator On", [&] { phazite::setModulatorOn (context, lfo, true); });
    asBefore::setOn (lfoBefore, true);
    CHECK (written (song, lfo) == asWhole (lfoBefore).toXmlString());

    // What a macro modulates: made, a second, the first's amount changed in its place, the first taken away.
    undo.check ("Modulate Volume", [&] { phazite::modulate (context, macro, volume, 0.5f); });
    asBefore::setModulation (macroBefore, volume, 0.5f);
    CHECK (written (song, macro) == asWhole (macroBefore).toXmlString());
    undo.check ("Modulate Pan", [&] { phazite::modulate (context, macro, pan, -0.123456f); });
    asBefore::setModulation (macroBefore, pan, -0.123456f);
    CHECK (written (song, macro) == asWhole (macroBefore).toXmlString());
    undo.check ("Change Modulation", [&] { phazite::modulate (context, macro, volume, -0.3f); });
    asBefore::setModulation (macroBefore, volume, -0.3f);
    CHECK (written (song, macro) == asWhole (macroBefore).toXmlString());
    undo.check ("Remove Modulation", [&] { phazite::removeModulation (context, macro, volume); });
    asBefore::removeModulation (macroBefore, volume);
    CHECK (written (song, macro) == asWhole (macroBefore).toXmlString());

    // A set as the song holds it: no action, no step; each edit that changes nothing, the same.
    {
        const phazite::UndoStep step (song, "Nothing");

        for (const auto id : { lfo, steps, macro })
            CHECK_FALSE (phazite::setModulator (song, *phazite::getModulatorValue (song, id)));

        CHECK_FALSE (phazite::setModulatorOrder (song, { .track = track }, { lfo, steps, macro }));
        phazite::setModulatorOn (song, lfo, true);
        phazite::setStep (context, steps, 3, 0.25f);
        phazite::renameMacro (song, macro, "Air");
        phazite::removeModulation (song, macro, volume);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }

    // Removed: gone, with what it modulates.
    undo.check ("Remove Macro", [&] { phazite::removeModulator (context, macro); });
    CHECK_FALSE (phazite::getModulatorValue (song, macro).has_value());
    CHECK_FALSE (findModulatorIn (phazite::test::copyWholeSong (song), macro).isValid());
    undo.walkBackAndForth();
}

TEST_CASE ("Modulation's setters: a modulator added at a place in its owner's list, moved to another owner, an owner's put in "
           "order among its other elements, one taken out")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto first = s.addModulator (track, ModulatorType::lfo);
    const auto second = s.addModulator (track, ModulatorType::random);
    const auto eq = s.addEffect (track, "phaziteEq"); // a device after them
    REQUIRE (eq.isValid());
    const ModulatorPlace owner { .track = track };
    phazite::test::UndoCheck undo (s.context);

    // The track's other children, in their order, which the setters leave as they are.
    const auto othersOf = [&song] (TrackID id)
    {
        juce::StringArray others;
        const auto element = phazite::test::copyTrack (song, id);

        for (const auto& child : element)
            if (! child.hasType (schema::MODULATOR) && ! child.hasType (schema::MACRO))
                others.add (child.getType().toString() + ":" + schema::getElementKey (child));

        return others;
    };

    const auto othersBefore = othersOf (track);

    // A new one, made whole, before the first; another after the others (-1), past the device.
    ModulatorValue made;
    made.id = ModulatorID::fromRaw (song.makeID());
    made.place = owner;
    made.kind = phazite::devices::ModulatorKind::breakpoints;
    made.off = true;
    made.modulations = { { .parameter = s.getPanParameter (track), .amount = 0.5f } };
    undo.check ("Add One", inStep (song, "Add One", [&] { CHECK (phazite::setModulator (song, made, 0)); }));
    CHECK (idsOf (song, owner) == std::vector { made.id, first, second });
    CHECK (phazite::getModulatorValue (song, made.id) == made);
    CHECK (written (song, made.id) == phazite::writeModulatorElement (made).toXmlString());

    auto last = made;
    last.id = ModulatorID::fromRaw (song.makeID());
    undo.check ("Add Another", inStep (song, "Add Another", [&] { CHECK (phazite::setModulator (song, last)); }));
    CHECK (idsOf (song, owner) == std::vector { made.id, first, second, last.id });
    CHECK (isLastChild (song, last.id));
    CHECK (othersOf (track) == othersBefore);

    // In another order: among the places they are at, the device where it is.
    undo.check ("Reorder",
                inStep (song, "Reorder", [&] { CHECK (phazite::setModulatorOrder (song, owner, { second, last.id, first, made.id })); }));
    CHECK (idsOf (song, owner) == std::vector { second, last.id, first, made.id });
    CHECK (othersOf (track) == othersBefore);
    CHECK (isLastChild (song, made.id));

    // To another track, second in its list there; its values as they were.
    auto moved = *phazite::getModulatorValue (song, second);
    moved.place = { .track = other };
    const auto onOther = s.addModulator (other, ModulatorType::lfo);
    const auto then = s.addModulator (other, ModulatorType::steps);
    undo.check ("Move", inStep (song, "Move", [&] { CHECK (phazite::setModulator (song, moved, 1)); }));
    CHECK (idsOf (song, { .track = other }) == std::vector { onOther, second, then });
    CHECK (idsOf (song, owner) == std::vector { last.id, first, made.id });
    CHECK (phazite::getModulatorValue (song, second) == moved);

    // Taken out; then nothing to take.
    undo.check ("Erase", inStep (song, "Erase", [&] { CHECK (phazite::eraseModulator (song, first)); }));
    CHECK_FALSE (phazite::getModulatorValue (song, first).has_value());
    CHECK_FALSE (phazite::eraseModulator (song, first));

    // A place the song hasn't: nothing.
    auto nowhere = made;
    nowhere.id = ModulatorID::fromRaw (song.makeID());
    nowhere.place = { .track = TrackID::fromRaw (song.makeID()) };
    CHECK_FALSE (phazite::setModulator (song, nowhere));
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("A modulator or a macro removed takes the MIDI links of what it was with it, which come back with its Undo (known bug "
           "241)")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto macro = s.addModulator (track, ModulatorType::macro);
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    const phazite::MidiController ofMacro { 1, 20 }, ofLfo { 1, 21 };

    {
        const phazite::UndoStep step (song, "Link MIDI Controllers");
        phazite::linkMidiController (song, ofMacro, valueOf (macro));
        phazite::linkMidiController (song, ofLfo, parameterOf (lfo, "rate"));
    }

    REQUIRE (phazite::findLinkedParameter (song, ofMacro) == valueOf (macro));
    REQUIRE (phazite::findLinkedParameter (song, ofLfo) == parameterOf (lfo, "rate"));
    phazite::test::UndoCheck undo (s.context);

    for (const auto& [removed, controller, keptController] : { std::tuple { macro, ofMacro, ofLfo }, std::tuple { lfo, ofLfo, ofMacro } })
    {
        undo.check ("Remove", [&] { phazite::removeModulator (s.context, removed); });
        CHECK_FALSE (phazite::getModulatorValue (song, removed).has_value());
        CHECK_FALSE (phazite::findLinkedParameter (song, controller).has_value());
        CHECK (phazite::findLinkedParameter (song, keptController).has_value());
        phazite::test::checkSong (song);

        phazite::undo (song);
        CHECK (phazite::getModulatorValue (song, removed).has_value());
        CHECK (phazite::findLinkedParameter (song, controller).has_value());
        phazite::redo (song);
        CHECK_FALSE (phazite::findLinkedParameter (song, controller).has_value());
        phazite::undo (song);
    }

    undo.walkBackAndForth();
}

TEST_CASE ("A duplicated track's modulators and macros are the copy's, modulating its devices and fader, and a macro's "
           "automation goes with its copy, a container's macro's too")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto synth = s.setInstrument (track, "phaziteSynth");
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    const auto macro = s.addModulator (track, ModulatorType::macro);
    s.modulate (lfo, { synth, "cutoff" }, 0.5f);
    s.modulate (macro, s.getVolumeParameter (track), 0.25f);
    s.setMacroValue (macro, 0.5f);
    const auto container = phazite::addContainer (s.context, track);
    REQUIRE (container.isValid());
    const auto containerMacro = phazite::getContainerMacros (song, container)[1];
    s.modulate (containerMacro, { phazite::getChains (song, container).front(), schema::chainVolumeParameter }, -0.5f);

    {
        const phazite::UndoStep step (song, "Automate Macro");
        phazite::addPoint (song, valueOf (macro), { phazite::BeatPosition::fromBeats (0.0), 0.2f, 0.0f });
        phazite::addPoint (song, valueOf (macro), { phazite::BeatPosition::fromBeats (4.0), 0.8f, 0.0f });
        phazite::addPoint (song, valueOf (containerMacro), { phazite::BeatPosition::fromBeats (2.0), 0.6f, 0.0f });
    }

    const auto curve = phazite::getCurveValue (song, valueOf (macro));
    REQUIRE (curve.has_value());
    REQUIRE (curve->points.size() == 2);
    const auto containerCurve = phazite::getCurveValue (song, valueOf (containerMacro));
    REQUIRE (containerCurve.has_value());
    phazite::test::UndoCheck undo (s.context);
    std::vector<TrackID> copies;
    undo.check ("Duplicate Track", inStep (song, "Duplicate Track", [&] { copies = phazite::duplicateTracks (song, { track }, nullptr); }));
    REQUIRE (copies.size() == 1);
    const auto copy = copies.front();

    // The copy's own, new IDs, set as the originals, modulating the copy's synth and fader.
    const auto copied = phazite::getModulators (song, copy);
    REQUIRE (copied.size() == 2);
    const auto copySynth = phazite::getInstrument (s.context, copy);
    REQUIRE (copySynth.has_value());
    const auto ofLfo = phazite::getModulations (song, copied[0]), ofMacro = phazite::getModulations (song, copied[1]);
    REQUIRE (ofLfo.size() == 1);
    CHECK (ofLfo[0].parameter == ParameterRef { copySynth->id, "cutoff" });
    REQUIRE (ofMacro.size() == 1);
    CHECK (ofMacro[0].parameter == s.getVolumeParameter (copy));
    CHECK (copied[0] != lfo);
    CHECK (copied[1] != macro);
    CHECK (phazite::getModulatorValue (song, copied[1])->value == doctest::Approx (0.5f));
    CHECK (phazite::getModulations (song, lfo).size() == 1);

    // The copied macro's value automated as the original's is, under its own ID.
    const auto copiedCurve = phazite::getCurveValue (song, valueOf (copied[1]));
    REQUIRE (copiedCurve.has_value());
    CHECK (*copiedCurve == *curve);
    CHECK (phazite::getCurveValue (song, valueOf (macro)) == curve);

    // And the copied container's macro's, under its copy's ID, modulating the copy's chain.
    std::vector<DeviceID> copiedContainers;

    for (const auto device : phazite::getDevices (song, copy))
        if (phazite::isContainer (song, device))
            copiedContainers.push_back (device);

    REQUIRE (copiedContainers.size() == 1);
    const auto copiedMacros = phazite::getContainerMacros (song, copiedContainers.front());
    REQUIRE (copiedMacros.size() == phazite::getContainerMacros (song, container).size());
    CHECK (copiedMacros[1] != containerMacro);
    const auto copiedContainerCurve = phazite::getCurveValue (song, valueOf (copiedMacros[1]));
    REQUIRE (copiedContainerCurve.has_value());
    CHECK (*copiedContainerCurve == *containerCurve);
    const auto ofCopiedMacro = phazite::getModulations (song, copiedMacros[1]);
    REQUIRE (ofCopiedMacro.size() == 1);
    CHECK (ofCopiedMacro[0].parameter
           == ParameterRef { phazite::getChains (song, copiedContainers.front()).front(), schema::chainVolumeParameter });
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("A duplicated chain is modulated as its original by its container's macros and its track's modulators, one exact "
           "undo step")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto container = phazite::addContainer (context, track);
    REQUIRE (container.isValid());
    const auto chain = phazite::getChains (song, container).front();
    phazite::addEffectToChain (context, chain, s.findDeviceType ("phaziteEq"));
    const auto eq = phazite::getChainDevices (song, chain).front();
    const ParameterRef gain { eq, "band4Gain" };
    const auto macro = phazite::getContainerMacros (song, container)[0];
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    s.modulate (macro, gain, 0.5f);
    s.modulate (lfo, gain, -0.25f);
    const auto macrosBefore = phazite::getContainerMacros (song, container);
    phazite::test::UndoCheck undo (context);

    DeviceID copy;
    undo.check ("Duplicate Chain", inStep (song, "Duplicate Chain", [&] { copy = phazite::duplicateChain (song, chain); }));
    REQUIRE (copy.isValid());
    const ParameterRef copiedGain { phazite::getChainDevices (song, copy).front(), "band4Gain" };
    const auto modulations = phazite::getModulations (song, copiedGain);
    REQUIRE (modulations.size() == 2);
    CHECK (modulations[0].modulator == lfo);
    CHECK (modulations[0].amount == doctest::Approx (-0.25f));
    CHECK (modulations[1].modulator == macro);
    CHECK (modulations[1].amount == doctest::Approx (0.5f));

    // No macro copied: a container's macros are the container's, which the copy is in.
    CHECK (phazite::getContainerMacros (song, container) == macrosBefore);
    CHECK (phazite::getModulations (song, gain).size() == 2);
    undo.walkBackAndForth();
}

TEST_CASE ("A folder made a bus and back keeps what modulated its volume, renamed in its place; what modulated the bus's pan, "
           "which a plain folder hasn't, goes")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto bus = s.addBus ("Bus", { s.firstTrack });
    const auto lfo = s.addModulator (bus, ModulatorType::lfo);
    s.modulate (lfo, s.getPanParameter (bus), 0.25f);
    s.modulate (lfo, s.getVolumeParameter (bus), 0.5f);
    const auto volumeIndex = [&song, lfo]
    {
        // Its place among the modulator's modulations, as the song holds them (and writes them, in their order).
        const auto modulations = phazite::getModulatorValue (song, lfo)->modulations;
        int index = -1;

        for (size_t i = 0; i < modulations.size(); ++i)
            if (std::abs (modulations[i].amount - 0.5f) < 1.0e-6f)
                index = (int) i;

        return index;
    };

    const auto placeBefore = volumeIndex();
    phazite::test::UndoCheck undo (s.context);

    undo.check ("Make Plain Folder", inStep (song, "Make Plain Folder", [&] { phazite::makePlainFolder (song, bus); }));
    auto modulations = phazite::getModulatorValue (song, lfo)->modulations;
    REQUIRE (modulations.size() == 1);
    CHECK (modulations[0].parameter == s.getVolumeParameter (bus)); // the folder's own volume now
    CHECK (modulations[0].amount == doctest::Approx (0.5f));

    undo.check ("Make Bus", inStep (song, "Make Bus", [&] { phazite::makeBus (song, bus); }));
    modulations = phazite::getModulatorValue (song, lfo)->modulations;
    REQUIRE (modulations.size() == 1);
    CHECK (modulations[0].parameter == s.getVolumeParameter (bus)); // the bus's fader's again

    // Undone, the pan's modulation back in its place, before the volume's, which kept its element where it was.
    phazite::undo (song);
    phazite::undo (song);
    CHECK (phazite::getModulatorValue (song, lfo)->modulations.size() == 2);
    CHECK (volumeIndex() == placeBefore);
    undo.walkBackAndForth();
}

TEST_CASE ("The versions make the song's modulation like a version's through modulation's setters: applied each way, a track's "
           "line, the song as copied")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto second = s.addTrack ("Second");
    const auto synth = s.setInstrument (track, "phaziteSynth");
    const auto eq = s.addEffect (track, "phaziteEq");
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    const auto steps = s.addModulator (track, ModulatorType::steps);
    const auto macro = s.addModulator (track, ModulatorType::macro);
    const auto onSecond = s.addModulator (second, ModulatorType::random);
    const auto container = phazite::addContainer (context, track);
    REQUIRE (container.isValid());
    const auto containerMacro = phazite::getContainerMacros (song, container)[2];
    s.modulate (lfo, { synth, "cutoff" }, 0.5f);
    s.modulate (macro, s.getVolumeParameter (track), 0.25f);
    s.modulate (macro, { eq, "band4Gain" }, -0.5f);
    s.modulate (onSecond, s.getPanParameter (second), 0.75f);
    const auto version = phazite::test::copyWholeSong (song);

    // The song since: a modulator added, one removed, the track's in another order, a value, a step, a modulation added,
    // one removed, an amount, a container's macro turned and named, the EQ (which the macro modulates) removed, the second
    // track's modulator changed.
    {
        const phazite::UndoStep step (song, "Since");
        phazite::addModulator (song, track, ModulatorType::breakpoints);
        phazite::removeModulator (song, steps);
        phazite::setModulatorOrder (song, { .track = track }, { macro, lfo });
        phazite::setValueSetByHand (song, parameterOf (lfo, "rate"), 3.0f);
        phazite::setModulation (song, lfo, s.getPanParameter (track), -0.25f);
        phazite::setModulation (song, macro, s.getVolumeParameter (track), 0.9f);
        phazite::removeModulation (song, lfo, { synth, "cutoff" });
        phazite::setValueSetByHand (song, valueOf (containerMacro), 0.4f);
        phazite::renameMacro (song, containerMacro, "Space");
        phazite::removeDevice (song, eq);
        phazite::setModulatorOn (song, onSecond, false);
    }

    const auto since = phazite::test::copyWholeSong (song);
    REQUIRE_FALSE (phazite::holdTheSame (since, version));
    phazite::test::UndoCheck undo (context);
    const auto apply = [&song] (const juce::ValueTree& other)
    {
        phazite::makeSongLike (song, other);
        phazite::removeBrokenReferences (song);
    };

    undo.check ("Bring Back", inStep (song, "Bring Back", [&] { apply (version); }));
    CHECK (phazite::holdTheSame (phazite::test::copyWholeSong (song), version));
    CHECK (idsOf (song, { .track = track }) == std::vector { lfo, steps, macro });
    phazite::test::checkSong (song);

    undo.check ("Bring Back Again", inStep (song, "Bring Back Again", [&] { apply (since); }));
    CHECK (phazite::holdTheSame (phazite::test::copyWholeSong (song), since));
    phazite::test::checkSong (song);

    // The first track's line alone put back: its modulators and its container's macros as the version holds them, the
    // second track's as they are now.
    undo.check ("Restore Track", inStep (song, "Restore Track", [&] { phazite::makeTrackLike (song, track, version); }));
    const auto steppedBack = phazite::getModulatorValue (song, steps);
    REQUIRE (steppedBack.has_value());
    CHECK (idsOf (song, { .track = track }) == std::vector { lfo, steps, macro });
    CHECK (phazite::getModulatorValue (song, containerMacro)->name == juce::String ("Macro 3"));
    CHECK (phazite::isModulatorOn (song, lfo));
    CHECK_FALSE (phazite::isModulatorOn (song, onSecond));

    // The song as copied after an edit undone: nothing of modulation written (asserted in Debug builds: ApplySong.cpp).
    const auto copy = phazite::test::copyWholeSong (song);
    phazite::setModulatorOn (context, lfo, false);
    phazite::undo (song);
    phazite::makeSongAsCopied (song, copy);
    CHECK (phazite::holdTheSame (phazite::test::copyWholeSong (song), copy));
    undo.walkBackAndForth();
}

TEST_CASE ("A folder's line put back brings its tracks back with their modulators and their containers' macros, and the master's "
           "line its own")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto inside = s.firstTrack;
    const auto synth = s.setInstrument (inside, "phaziteSynth");
    const auto folder = s.addBus ("Folder", { inside });
    const auto lfo = s.addModulator (inside, ModulatorType::lfo);
    s.modulate (lfo, { synth, "cutoff" }, 0.5f);
    const auto onFolder = s.addModulator (folder, ModulatorType::random);
    s.modulate (onFolder, s.getVolumeParameter (folder), 0.25f);
    const auto container = phazite::addContainer (context, inside);
    REQUIRE (container.isValid());
    const auto containerMacro = phazite::getContainerMacros (song, container)[0];
    s.modulate (containerMacro, { phazite::getChains (song, container).front(), schema::chainVolumeParameter }, -0.5f);
    s.setMacroValue (containerMacro, 0.25f);
    // The master's: an LFO and a container's macro, each modulating the container's chain.
    const auto master = phazite::getMasterID (song);
    const auto onMaster = s.addModulator (master, ModulatorType::lfo);
    const auto masterContainer = phazite::addContainer (context, master);
    REQUIRE (masterContainer.isValid());
    const auto masterMacro = phazite::getContainerMacros (song, masterContainer)[0];
    const auto masterChain = phazite::getChains (song, masterContainer).front();
    const ParameterRef masterPan { masterChain, schema::chainPanParameter };
    s.modulate (onMaster, masterPan, 0.5f);
    s.modulate (masterMacro, { masterChain, schema::chainVolumeParameter }, 0.25f);
    const auto version = phazite::test::copyWholeSong (song);
    phazite::test::UndoCheck undo (context);

    // The folder deleted with the track in it, both put back by the folder's line: the song the version's again, the track's
    // modulator and its container's macros with it.
    undo.check ("Delete Folder", inStep (song, "Delete Folder", [&] { phazite::deleteTracks (song, { folder }, nullptr); }));
    REQUIRE_FALSE (phazite::getModulatorValue (song, lfo).has_value());
    undo.check ("Restore Folder", inStep (song, "Restore Folder",
                                          [&]
                                          {
                                              REQUIRE (phazite::makeTrackLike (song, folder, version));
                                              phazite::removeBrokenReferences (song);
                                          }));
    CHECK (phazite::holdTheSame (phazite::test::copyWholeSong (song), version));
    CHECK (idsOf (song, { .track = inside }) == std::vector { lfo });
    CHECK (idsOf (song, { .track = inside, .container = container }) == phazite::getContainerMacros (song, container));
    REQUIRE (phazite::getModulatorValue (song, containerMacro).has_value());
    CHECK (phazite::getModulatorValue (song, containerMacro)->value == doctest::Approx (0.25f));
    CHECK (phazite::getModulations (song, containerMacro).size() == 1);
    phazite::test::checkSong (song);

    // The master's line: its modulators as the version holds them, a track's as they are now.
    {
        const phazite::UndoStep step (song, "Since");
        phazite::setModulatorOn (song, onMaster, false);
        phazite::setModulation (song, onMaster, masterPan, -0.25f);
        phazite::addModulator (song, master, ModulatorType::steps);
        phazite::renameMacro (song, masterMacro, "Width");
        phazite::setModulatorOn (song, lfo, false);
    }

    undo.check ("Restore Master", inStep (song, "Restore Master", [&] { REQUIRE (phazite::makeTrackLike (song, master, version)); }));
    CHECK (idsOf (song, { .onMaster = true }) == std::vector { onMaster });
    CHECK (phazite::isModulatorOn (song, onMaster));
    REQUIRE (phazite::getModulatorValue (song, onMaster)->findModulation (masterPan) != nullptr);
    CHECK (phazite::getModulatorValue (song, onMaster)->findModulation (masterPan)->amount == doctest::Approx (0.5f));
    CHECK (phazite::getModulatorValue (song, masterMacro)->name == juce::String ("Macro 1"));
    CHECK_FALSE (phazite::isModulatorOn (song, lfo));
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("A line put back takes away the modulators and macros added to it since with their curves, the master's line the "
           "master's")
{
    // The line's track's own element holds no MODULATOR or MACRO, which the song holds in modulation's area: the curves of
    // what the line takes away are found there (ApplySong.cpp: makeTrackLikeIn()).
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto master = phazite::getMasterID (song);
    const auto version = phazite::test::copyWholeSong (song);

    // Since: an LFO on the track, a container on it, an LFO on the master, each with a curve (the LFOs' rates, the
    // container's first macro's value).
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    const auto container = phazite::addContainer (context, track);
    REQUIRE (container.isValid());
    const auto macro = phazite::getContainerMacros (song, container)[0];
    const auto onMaster = s.addModulator (master, ModulatorType::lfo);
    const std::vector<ParameterRef> onTrack { parameterOf (lfo, "rate"), valueOf (macro) };
    const auto masterRate = parameterOf (onMaster, "rate");

    {
        const phazite::UndoStep step (song, "Since");

        for (const auto& parameter : { onTrack[0], onTrack[1], masterRate })
            phazite::addPoint (song, parameter, { .beat = {}, .position = 0.5f, .curve = 0.0f });
    }

    const auto hasCurves = [&song] (const std::vector<ParameterRef>& parameters)
    {
        return std::all_of (parameters.begin(), parameters.end(),
                            [&song] (const ParameterRef& parameter) { return phazite::findCurve (song, parameter) != nullptr; });
    };
    const auto hasNoCurves = [&song] (const std::vector<ParameterRef>& parameters)
    {
        return std::none_of (parameters.begin(), parameters.end(),
                             [&song] (const ParameterRef& parameter) { return phazite::findCurve (song, parameter) != nullptr; });
    };
    const auto trackHolds = [&]
    { return phazite::getModulatorValue (song, lfo).has_value() && phazite::getModulatorValue (song, macro).has_value(); };
    REQUIRE (hasCurves (onTrack));
    REQUIRE (hasCurves ({ masterRate }));
    phazite::test::UndoCheck undo (context);

    // The track's line: its LFO, its container and their curves gone; the master's as they are.
    undo.check ("Restore Track", inStep (song, "Restore Track", [&] { REQUIRE (phazite::makeTrackLike (song, track, version)); }));
    CHECK_FALSE (phazite::getModulatorValue (song, lfo).has_value());
    CHECK_FALSE (phazite::getModulatorValue (song, macro).has_value());
    CHECK_FALSE (phazite::hasDevice (song, container));
    CHECK (hasNoCurves (onTrack));
    CHECK (hasCurves ({ masterRate }));
    phazite::test::checkSong (song);

    phazite::undo (song);
    CHECK (trackHolds());
    CHECK (phazite::hasDevice (song, container));
    CHECK (hasCurves (onTrack));
    phazite::redo (song);
    CHECK_FALSE (phazite::getModulatorValue (song, lfo).has_value());
    CHECK (hasNoCurves (onTrack));

    // The master's line: its LFO and its curve gone.
    undo.check ("Restore Master", inStep (song, "Restore Master", [&] { REQUIRE (phazite::makeTrackLike (song, master, version)); }));
    CHECK_FALSE (phazite::getModulatorValue (song, onMaster).has_value());
    CHECK (hasNoCurves ({ masterRate }));
    phazite::test::checkSong (song);

    phazite::undo (song);
    CHECK (phazite::getModulatorValue (song, onMaster).has_value());
    CHECK (hasCurves ({ masterRate }));
    phazite::redo (song);
    CHECK (hasNoCurves ({ masterRate }));
    undo.walkBackAndForth();
}

TEST_CASE ("A folder's line put back brings its tracks' curves back with them")
{
    // A folder deleted takes the tracks in it, and their curves; its line put back brings the tracks back (ApplySong.cpp:
    // makeTrackLikeIn(), the tracks that come back with it), and their devices', modulators' and sends' curves with them.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto inside = s.firstTrack;
    const auto synth = s.setInstrument (inside, "phaziteSynth");
    const auto folder = s.addBus ("Folder", { inside });
    const auto macro = s.addModulator (inside, ModulatorType::macro);
    const std::vector<ParameterRef> curves { { synth, "cutoff" }, valueOf (macro), s.getVolumeParameter (inside) };

    {
        const phazite::UndoStep step (song, "Curves");

        for (const auto& parameter : curves)
            phazite::addPoint (song, parameter, { .beat = {}, .position = 0.25f, .curve = 0.0f });
    }

    const auto version = phazite::test::copyWholeSong (song);
    phazite::test::UndoCheck undo (context);

    // Since: each curve given a second point, then the folder deleted with the track in it.
    undo.check ("Since",
                inStep (song, "Since",
                        [&]
                        {
                            for (const auto& parameter : curves)
                                phazite::addPoint (song, parameter,
                                                   { .beat = phazite::BeatPosition::fromBeats (4.0), .position = 0.75f, .curve = 0.0f });
                        }));
    undo.check ("Delete Folder", inStep (song, "Delete Folder", [&] { phazite::deleteTracks (song, { folder }, nullptr); }));

    // Put back: each curve as the version holds it, one point.
    undo.check ("Restore Folder", inStep (song, "Restore Folder", [&] { REQUIRE (phazite::makeTrackLike (song, folder, version)); }));

    for (const auto& parameter : curves)
    {
        INFO (parameter.parameter.toStdString());
        const auto* curve = phazite::findCurve (song, parameter);
        REQUIRE (curve != nullptr);
        CHECK (curve->points.size() == 1);
    }

    CHECK (phazite::holdTheSame (phazite::test::copyWholeSong (song), version));
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("A channel's modulators copied onto another (a bounce's) are set as their originals, their copies' IDs among "
           "the copied, one exact undo step")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto from = s.firstTrack;
    const auto to = s.addTrack ("To");
    const auto synth = s.setInstrument (from, "phaziteSynth");
    const auto lfo = s.addModulator (from, ModulatorType::lfo);
    const auto macro = s.addModulator (from, ModulatorType::macro);
    const auto other = s.addModulator (from, ModulatorType::random);
    s.setModulatorParameter (lfo, "rate", 2.0f);
    s.modulate (lfo, s.getVolumeParameter (from), 0.5f);
    s.modulate (lfo, { synth, "cutoff" }, 0.25f);
    s.modulate (macro, s.getPanParameter (from), -0.25f);
    s.setMacroValue (macro, 0.5f);
    s.modulate (other, { synth, "cutoff" }, 0.5f); // modulates what isn't copied alone: stays behind
    phazite::CopiedIDs copied { { s.getVolumeParameter (from).device.getRaw(), s.getVolumeParameter (to).device.getRaw() } };
    phazite::test::UndoCheck undo (s.context);

    undo.check ("Copy Modulators", inStep (song, "Copy Modulators", [&] { phazite::copyModulators (song, from, to, copied); }));
    const auto copies = phazite::getModulators (song, to);
    REQUIRE (copies.size() == 2);
    CHECK (copied.at (lfo.getRaw()) == copies[0].getRaw());
    CHECK (copied.at (macro.getRaw()) == copies[1].getRaw());
    CHECK (copied.count (other.getRaw()) == 0);

    // Each as its original, but for its ID, its place and what it modulates: the copies of what it modulated that was copied.
    for (const auto& [original, copy] : { std::pair { lfo, copies[0] }, std::pair { macro, copies[1] } })
    {
        auto expected = *phazite::getModulatorValue (song, original);
        const auto made = phazite::getModulatorValue (song, copy);
        REQUIRE (made.has_value());
        REQUIRE (made->modulations.size() == 1);
        expected.id = made->id;
        expected.place = ModulatorPlace { .track = to };
        expected.modulations = made->modulations;
        CHECK (*made == expected);
    }

    CHECK (phazite::getModulations (song, copies[0])[0].parameter == s.getVolumeParameter (to));
    CHECK (phazite::getModulations (song, copies[1])[0].parameter == s.getPanParameter (to));
    CHECK (phazite::getModulators (song, from).size() == 3);
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("A song opened from a file whose modulators it holds otherwise than it reads them: each held as read, a version of it "
           "applied writing nothing more, an edit of one value writing that value alone")
{
    // The loader makes each modulator and macro as the song holds it (Project.h: makeConsistent(); ModulationValue.h:
    // keepModulatorAsHeld()): else a version's pass, which reads the version's as the song holds them and the song's as its
    // code reads them, wrote at every apply, and an edit rewrote what it didn't change.
    phazite::test::TestSong s;
    const auto track = s.firstTrack;
    const auto synth = s.setInstrument (track, "phaziteSynth");
    auto state = phazite::test::copyWholeSong (*phazite::findSong (s.context));
    auto trackState = state.getChildWithName (schema::TRACKS).getChildWithProperty (schema::trackID, (juce::int64) track.getRaw());
    REQUIRE (trackState.isValid());
    const auto device = juce::String (synth.getRaw());

    for (const auto& text :
         { R"(<MODULATOR id="880001" type="lfo" shape="round"><MODULATES device=")" + device
               + R"(" parameter="cutoff" amount="0.123456"/><MODULATES device=")" + device
               + R"(" parameter="" amount="0.5"/><VALUES rate="1" depth="0.5" bipolar="Bipolar" wave="2"/><OTHER/></MODULATOR>)",
           juce::String (R"(<MODULATOR id="880002" type="steps" steps="0.123456 0.5 0 0"/>)"),
           R"(<MACRO id="880003" name="Air" value="0.30"><MODULATES device=")" + device
               + R"(" parameter="resonance" amount="0.25"/></MACRO>)" })
        trackState.appendChild (juce::ValueTree::fromXml (text), nullptr);

    s.openSong (state);
    auto& song = *phazite::findSong (s.context);
    const auto lfo = ModulatorID::fromRaw (880001), steps = ModulatorID::fromRaw (880002), macro = ModulatorID::fromRaw (880003);

    for (const auto id : { lfo, steps, macro })
    {
        const auto whole = phazite::test::copyWholeSong (song);
        const auto element = findModulatorIn (whole, id);
        REQUIRE (element.isValid());
        const auto place = phazite::readModulatorPlace (element);
        const auto held = phazite::readHeldModulator (element, place);
        REQUIRE (held.has_value());
        CHECK (phazite::readModulatorValue (element, place) == held);
    }

    // The song as copied, and a version of it applied, write nothing (the first asserted in Debug builds: ApplySong.cpp).
    const auto version = phazite::test::copyWholeSong (song);
    phazite::makeSongAsCopied (song, version);

    {
        const phazite::UndoStep step (song, "Apply");
        phazite::makeSongLike (song, version);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }

    // An edit of one value: one action, the rest of its element as it was.
    for (const auto& [id, parameter, value] :
         { std::tuple { lfo, juce::String ("rate"), 3.0f }, std::tuple { macro, valueOf (macro).parameter, 0.5f } })
    {
        auto expected = findModulatorIn (phazite::test::copyWholeSong (song), id).createCopy();

        {
            const phazite::UndoStep step (song, "Change");
            phazite::setValueSetByHand (song, parameterOf (id, parameter), value);
            CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 1);
        }

        const auto element = findModulatorIn (phazite::test::copyWholeSong (song), id);

        if (id == lfo)
            expected.getChildWithName (schema::VALUES).setProperty ("rate", element.getChildWithName (schema::VALUES)["rate"], nullptr);
        else
            expected.setProperty (schema::macroValue, element[schema::macroValue], nullptr);

        CHECK (element.toXmlString() == asWhole (expected).toXmlString());
    }

    // The version applied each way, and made the song's whole: the song then holds the same as the other, and a second apply
    // writes nothing.
    const auto since = phazite::test::copyWholeSong (song);

    for (const auto* other : { &version, &since, &version })
    {
        {
            const phazite::UndoStep step (song, "Apply");
            phazite::makeSongLike (song, *other);
            phazite::removeBrokenReferences (song);
        }

        CHECK (phazite::holdTheSame (phazite::test::copyWholeSong (song), *other));
        const phazite::UndoStep step (song, "Again");
        phazite::makeSongLike (song, *other);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }

    phazite::makeSongTheSame (song, since);
    CHECK (phazite::holdTheSame (phazite::test::copyWholeSong (song), since));
    const phazite::UndoStep step (song, "Again");
    phazite::makeSongLike (song, since);
    CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
}

namespace
{
// A merge's clashes of a song whose two lines changed what one parted from: base, the song as it was; theirs, the other
// line's changes made from there; ours, this line's, made from there too, which the song holds after. Each clash chosen each
// way through modulation's chooser, in a step of its own, then undone.
struct Merge
{
    phazite::Song& song;
    juce::ValueTree base {}, ours {}, theirs {};
    std::vector<phazite::MergeClash> clashes {};

    Merge (phazite::Song& s, const std::function<void()>& theirChanges, const std::function<void()>& ourChanges)
        : song (s)
    {
        base = phazite::test::copyWholeSong (song);

        {
            const phazite::UndoStep step (song, "Theirs");
            theirChanges();
        }

        theirs = phazite::test::copyWholeSong (song);
        phazite::undo (song);

        {
            const phazite::UndoStep step (song, "Ours");
            ourChanges();
        }

        ours = phazite::test::copyWholeSong (song);
        clashes = phazite::mergeSongs (base, ours, theirs, [this] { return song.makeID(); }).clashes;
    }

    const phazite::MergeClash* find (const juce::String& pathEnd, const juce::Identifier& property = {}) const
    {
        for (const auto& clash : clashes)
            if (clash.path[clash.path.size() - 1] == pathEnd && clash.property == property)
                return &clash;

        return nullptr;
    }

    // The clash chosen with this value, checked, then undone.
    void choose (phazite::test::UndoCheck& undo, const phazite::MergeClash& clash, int value, const std::function<void()>& check)
    {
        // What the song holds already (this line's, mostly) writes nothing, and makes no step to undo.
        const auto before = phazite::test::copyWholeSong (song);
        auto choice = phazite::AreaChoice::notTheArea;
        undo.check ("Choose",
                    inStep (song, "Choose", [&] { choice = phazite::chooseModulationClash (song, clash, value, { base, ours, theirs }); }));
        CHECK_MESSAGE (choice == phazite::AreaChoice::chosen, "choice " << (int) choice << ", value " << value);

        if (choice != phazite::AreaChoice::chosen)
            return;

        check();
        phazite::test::checkSong (song);

        if (! phazite::test::copyWholeSong (song).isEquivalentTo (before))
            phazite::undo (song);
    }
};
} // namespace

TEST_CASE ("A merge's clash in modulation is chosen through modulation's setters, each kind each way, and a modulation both lines "
           "made goes for what was before (known bug 239's shape)")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto volume = s.getVolumeParameter (track), pan = s.getPanParameter (track);
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    const auto steps = s.addModulator (track, ModulatorType::steps);
    const auto macro = s.addModulator (track, ModulatorType::macro);
    s.modulate (lfo, volume, 0.5f);
    const auto rateIndex = phazite::devices::findModulatorParameter (phazite::devices::ModulatorKind::lfo, "rate");
    REQUIRE (rateIndex >= 0);

    // Each line: the LFO's rate, the macro's name and value, the step sequencer's steps and switch, the LFO's modulation
    // of the volume, and a modulation of the pan both made, by other amounts.
    Merge merge (
        song,
        [&]
        {
            phazite::setValueSetByHand (song, parameterOf (lfo, "rate"), 3.0f);
            phazite::renameMacro (song, macro, "Theirs");
            phazite::setModulatorStep (song, steps, 0, 0.5f);
            phazite::setModulatorOn (song, steps, false);
            phazite::setModulation (song, lfo, volume, -0.5f);
            phazite::setModulation (song, macro, pan, 0.75f);
        },
        [&]
        {
            phazite::setValueSetByHand (song, parameterOf (lfo, "rate"), 2.0f);
            phazite::renameMacro (song, macro, "Ours");
            phazite::setModulatorStep (song, steps, 0, -0.5f);
            phazite::setModulation (song, lfo, volume, 0.25f);
            phazite::setModulation (song, macro, pan, 0.25f);
            phazite::setValueSetByHand (song, valueOf (macro), 0.5f);
        });

    phazite::test::UndoCheck undo (s.context);
    const auto lfoStep = schema::MODULATOR.toString() + ":" + juce::String (lfo.getRaw());
    const auto macroStep = schema::MACRO.toString() + ":" + juce::String (macro.getRaw());
    const auto stepsStep = schema::MODULATOR.toString() + ":" + juce::String (steps.getRaw());
    const auto modulatesOf = [] (const ParameterRef& parameter)
    { return schema::MODULATES.toString() + ":" + juce::String (parameter.device.getRaw()) + "/" + parameter.parameter; };

    // A value set by hand (a property of the LFO's VALUES).
    const auto* rate = merge.find (schema::VALUES.toString() + ":" + schema::VALUES.toString(), "rate");
    REQUIRE (rate != nullptr);
    const auto rateDefault = infoOf (phazite::devices::ModulatorKind::lfo, "rate").defaultValue;

    for (const auto& [value, expected] : { std::pair { 0, rateDefault }, std::pair { 1, 2.0f }, std::pair { 2, 3.0f } })
        merge.choose (undo, *rate, value,
                      [&] { CHECK (phazite::getModulatorValue (song, lfo)->getParameterValue (rateIndex) == doctest::Approx (expected)); });

    // A macro's name.
    const auto* name = merge.find (macroStep, schema::macroName);
    REQUIRE (name != nullptr);

    for (const auto& [value, expected] : { std::pair { 0, "Macro 1" }, std::pair { 1, "Ours" }, std::pair { 2, "Theirs" } })
        merge.choose (undo, *name, value, [&] { CHECK (phazite::getModulatorValue (song, macro)->name == juce::String (expected)); });

    // A step sequencer's steps.
    const auto* stepsClash = merge.find (stepsStep, schema::modulatorSteps);
    REQUIRE (stepsClash != nullptr);

    for (const auto& [value, expected] : { std::pair { 0, -1.0f }, std::pair { 1, -0.5f }, std::pair { 2, 0.5f } })
        merge.choose (undo, *stepsClash, value,
                      [&] { CHECK (phazite::getModulatorValue (song, steps)->steps[0] == doctest::Approx (expected)); });

    // A modulation's amount both lines changed.
    const auto* amount = merge.find (modulatesOf (volume), schema::modulatesAmount);
    REQUIRE (amount != nullptr);

    for (const auto& [value, expected] : { std::pair { 0, 0.5f }, std::pair { 1, 0.25f }, std::pair { 2, -0.5f } })
        merge.choose (undo, *amount, value, [&]
                      { CHECK (phazite::getModulatorValue (song, lfo)->findModulation (volume)->amount == doctest::Approx (expected)); });

    // A modulation both lines made: what was before had none, so none (known bug 239's shape: not one of no amount).
    const auto* made = merge.find (modulatesOf (pan), schema::modulatesAmount);
    REQUIRE (made != nullptr);
    CHECK (made->base.isVoid());
    merge.choose (undo, *made, 0, [&] { CHECK (phazite::getModulatorValue (song, macro)->findModulation (pan) == nullptr); });
    merge.choose (undo, *made, 1,
                  [&] { CHECK (phazite::getModulatorValue (song, macro)->findModulation (pan)->amount == doctest::Approx (0.25f)); });
    merge.choose (undo, *made, 2,
                  [&] { CHECK (phazite::getModulatorValue (song, macro)->findModulation (pan)->amount == doctest::Approx (0.75f)); });

    // Taken away, then the other line's chosen: put back as it holds it.
    {
        const phazite::UndoStep step (song, "Before");
        REQUIRE (phazite::chooseModulationClash (song, *made, 0, { merge.base, merge.ours, merge.theirs }) == phazite::AreaChoice::chosen);
    }

    merge.choose (undo, *made, 2,
                  [&] { CHECK (phazite::getModulatorValue (song, macro)->findModulation (pan)->amount == doctest::Approx (0.75f)); });
    phazite::undo (song);

    // A path through a modulator the song hasn't, its ID, a parameter its kind hasn't: refused, nothing written.
    auto nowhere = *amount;
    nowhere.path.set (nowhere.path.size() - 2, schema::MODULATOR.toString() + ":" + juce::String (song.makeID()));
    auto byID = *name;
    byID.property = schema::modulatorID;
    auto unknown = *rate;
    unknown.property = "nothing";

    for (const auto* refused : { &nowhere, &byID, &unknown })
    {
        const phazite::UndoStep step (song, "Refused");
        CHECK (phazite::chooseModulationClash (song, *refused, 2, { merge.base, merge.ours, merge.theirs })
               == phazite::AreaChoice::refused);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }
}

TEST_CASE ("A merge's clash on a whole modulator, a whole modulation and a modulator's VALUES is chosen each way")
{
    // A merge reports what one line took away and the other changed as a note, keeping it (SongMerge.cpp), so these clashes
    // come only in a commit's song that isn't Phazite's (tests/FuzzTests.cpp): each made here as a merge names one, its path
    // the element's, its three elements each side's there.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto volume = s.getVolumeParameter (track);
    const auto lfo = s.addModulator (track, ModulatorType::lfo);
    const auto sequencer = s.addModulator (track, ModulatorType::steps);
    const auto random = s.addModulator (track, ModulatorType::random);
    s.modulate (lfo, volume, 0.5f);
    s.setModulatorParameter (sequencer, "depth", 0.5f);
    const auto depthIndex = phazite::devices::findModulatorParameter (phazite::devices::ModulatorKind::steps, "depth");
    REQUIRE (depthIndex >= 0);

    // The paths, from the song down, as a merge's clash names an element.
    const auto pathOf = [] (const juce::ValueTree& element)
    {
        juce::StringArray path;

        for (auto e = element; e.isValid() && e.getParent().isValid(); e = e.getParent())
            path.insert (0, e.getType().toString() + ":" + schema::getElementKey (e));

        return path;
    };

    const auto songWhole = phazite::test::copyWholeSong (song); // the song's tree holds no modulator: its paths are the whole's
    const auto lfoElement = findModulatorIn (songWhole, lfo);
    juce::ValueTree modulatesElement;

    for (const auto& child : lfoElement)
        if (child.hasType (schema::MODULATES))
            modulatesElement = child;

    const auto modulatesPath = pathOf (modulatesElement);
    const auto valuesPath = pathOf (findModulatorIn (songWhole, sequencer).getChildWithName (schema::VALUES));
    const auto randomPath = pathOf (findModulatorIn (songWhole, random));

    // Theirs changes what ours takes away: the LFO's modulation (its amount), the step sequencer's values (its depth, the one
    // it has: ours sets it back to its default, which takes its VALUES away), the random source (switched off there).
    Merge merge (
        song,
        [&]
        {
            phazite::setModulation (song, lfo, volume, -0.25f);
            phazite::setValueSetByHand (song, parameterOf (sequencer, "depth"), 0.25f);
            phazite::setModulatorOn (song, random, false);
        },
        [&]
        {
            phazite::removeModulation (song, lfo, volume);
            phazite::setValueSetByHand (song, parameterOf (sequencer, "depth"), 1.0f);
            phazite::removeModulator (song, random);
        });

    const auto clashOn = [&merge] (const juce::StringArray& path)
    {
        phazite::MergeClash clash;
        clash.line = "Track";
        clash.path = path;
        clash.baseElement = phazite::findByPath (merge.base, path);
        clash.oursElement = phazite::findByPath (merge.ours, path);
        clash.theirsElement = phazite::findByPath (merge.theirs, path);
        REQUIRE (clash.baseElement.isValid());
        REQUIRE (clash.theirsElement.isValid());
        CHECK_FALSE (clash.oursElement.isValid());
        return clash;
    };

    phazite::test::UndoCheck undo (s.context);
    const auto modulation = clashOn (modulatesPath);
    merge.choose (undo, modulation, 0,
                  [&] { CHECK (phazite::getModulatorValue (song, lfo)->findModulation (volume)->amount == doctest::Approx (0.5f)); });
    merge.choose (undo, modulation, 1, [&] { CHECK (phazite::getModulatorValue (song, lfo)->findModulation (volume) == nullptr); });
    merge.choose (undo, modulation, 2,
                  [&] { CHECK (phazite::getModulatorValue (song, lfo)->findModulation (volume)->amount == doctest::Approx (-0.25f)); });

    const auto values = clashOn (valuesPath);
    const auto depthOf = [&song, sequencer, depthIndex]
    { return phazite::getModulatorValue (song, sequencer)->getParameterValue (depthIndex); };
    merge.choose (undo, values, 0, [&] { CHECK (depthOf() == doctest::Approx (0.5f)); });
    merge.choose (undo, values, 1, [&] { CHECK (phazite::getModulatorValue (song, sequencer)->values.empty()); });
    merge.choose (undo, values, 2, [&] { CHECK (depthOf() == doctest::Approx (0.25f)); });

    const auto whole = clashOn (randomPath);
    merge.choose (undo, whole, 0, [&] { CHECK (phazite::isModulatorOn (song, random)); });
    merge.choose (undo, whole, 1, [&] { CHECK_FALSE (phazite::getModulatorValue (song, random).has_value()); });
    merge.choose (undo, whole, 2, [&] { CHECK_FALSE (phazite::isModulatorOn (song, random)); });

    // One of another ID than its path's: refused, nothing written.
    auto other = whole;
    other.theirsElement = phazite::findByPath (merge.theirs, modulatesPath).getParent();

    {
        const phazite::UndoStep step (song, "Refused");
        CHECK (phazite::chooseModulationClash (song, other, 2, { merge.base, merge.ours, merge.theirs }) == phazite::AreaChoice::refused);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }
}

TEST_CASE ("A merge's clash on a container's macro is chosen through modulation's setters, each way")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto container = phazite::addContainer (s.context, s.firstTrack);
    REQUIRE (container.isValid());
    const auto macro = phazite::getContainerMacros (song, container)[3];

    // Each line: the macro renamed and turned, each its own way.
    Merge merge (
        song,
        [&]
        {
            phazite::renameMacro (song, macro, "Theirs");
            phazite::setValueSetByHand (song, valueOf (macro), 0.75f);
        },
        [&]
        {
            phazite::renameMacro (song, macro, "Ours");
            phazite::setValueSetByHand (song, valueOf (macro), 0.25f);
        });

    phazite::test::UndoCheck undo (s.context);
    const auto macroStep = schema::MACRO.toString() + ":" + juce::String (macro.getRaw());
    const auto* name = merge.find (macroStep, schema::macroName);
    REQUIRE (name != nullptr);
    CHECK (name->path[name->path.size() - 2] == schema::CONTAINER.toString() + ":" + juce::String (container.getRaw()));

    for (const auto& [value, expected] : { std::pair { 0, "Macro 4" }, std::pair { 1, "Ours" }, std::pair { 2, "Theirs" } })
        merge.choose (undo, *name, value, [&] { CHECK (phazite::getModulatorValue (song, macro)->name == juce::String (expected)); });

    const auto* turned = merge.find (macroStep, schema::macroValue);
    REQUIRE (turned != nullptr);

    for (const auto& [value, expected] : { std::pair { 0, 0.0f }, std::pair { 1, 0.25f }, std::pair { 2, 0.75f } })
        merge.choose (undo, *turned, value, [&] { CHECK (phazite::getModulatorValue (song, macro)->value == doctest::Approx (expected)); });

    CHECK (phazite::getModulatorValue (song, macro)->place == ModulatorPlace { .track = s.firstTrack, .container = container });
    undo.walkBackAndForth();
}
