// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's parameters as Phazite describes them itself (src/model/devices/internal/Parameters.h), checked against
// Tracktion's parameters, which mirror them while Tracktion's Edit does (docs/tracktion-migration.md, T6 step 4): the
// same names, ranges, defaults and texts, so that what controls show, saved values and automation stay as they were.
#include "TestHelpers.h"
#include "engine/nodes/Channel.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Parameters.h"
#include "model/mirror/internal/Mirror.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/internal/Project.h"

namespace
{
using phazite::ParameterRef;

// Every parameter of a device of Tracktion's, described as Phazite describes the song's by the same reference.
void checkAgainstTracktion (const phazite::Song& song, phazite::DeviceID id, te::AutomatableParameter& parameter)
{
    const ParameterRef ref { id, parameter.paramID };
    INFO (parameter.paramID.toStdString());
    const auto description = phazite::describeParameter (song, ref);
    REQUIRE (description.has_value());

    CHECK (description->name == parameter.getParameterName());
    CHECK (description->range.start == doctest::Approx (parameter.valueRange.start));
    CHECK (description->range.end == doctest::Approx (parameter.valueRange.end));
    CHECK (description->range.interval == doctest::Approx (parameter.valueRange.interval));
    CHECK (description->range.skew == doctest::Approx (parameter.valueRange.skew));
    CHECK (description->defaultValue.has_value() == parameter.getDefaultValue().has_value());

    if (description->defaultValue.has_value() && parameter.getDefaultValue().has_value())
        CHECK (*description->defaultValue == doctest::Approx (*parameter.getDefaultValue()));

    // How values read, across the range; and a value set by hand stays as it is (Tracktion snaps none of these).
    for (const auto proportion : { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const auto value = parameter.valueRange.convertFrom0to1 (proportion);
        CHECK (phazite::getParameterText (song, ref, value) == parameter.valueToString (value));
        CHECK (parameter.snapToState (value) == doctest::Approx (value));
    }

    CHECK (phazite::getValueSetByHand (song, ref) == doctest::Approx (parameter.getCurrentExplicitValue()));
}

struct Song
{
    std::unique_ptr<te::Edit> edit = phazite::createSongEdit (phazite::test::getEngine());
    phazite::Song& song = phazite::getSong (*edit);
    std::vector<phazite::TrackID> tracks = phazite::ensureTracks (song, 2);

    te::Plugin& tracktions (phazite::DeviceID id) const
    {
        auto* device = phazite::mirror::findDevice (*edit, id);
        REQUIRE (device != nullptr);
        return *device;
    }
};
} // namespace

TEST_CASE ("Phazite's own devices' parameters are described as Tracktion's plug-ins around them describe them")
{
    Song s;

    // Instruments, each on a track of its own, and effects, with choices, switches, skewed ranges and percentages among their
    // parameters.
    phazite::DeviceID device;

    for (const auto* type : { "phaziteSynth", "phaziteDrums", "phaziteWavetable", "phaziteEq", "phaziteCompressor", "phaziteDelay",
                              "phaziteReverb", "phaziteFilter" })
    {
        const auto track = phazite::insertTrack (s.song, s.tracks[1], nullptr);
        device = phazite::insertDeviceState (s.song, track, phazite::makeOwnDeviceState (s.song, type));
        REQUIRE (device.isValid());
        INFO (std::string (type));

        for (auto* parameter : s.tracktions (device).getAutomatableParameters())
            checkAgainstTracktion (s.song, device, *parameter);
    }

    // What isn't one of a device's parameters is none.
    CHECK_FALSE (phazite::describeParameter (s.song, ParameterRef { device, "no such parameter" }).has_value());
}

TEST_CASE ("A fader's volume and pan, the master's, a plain folder's VCA and a send's level are described as Tracktion's")
{
    Song s;
    const auto check = [&s] (phazite::DeviceID id, const char* parameterID)
    {
        auto parameter = s.tracktions (id).getAutomatableParameterByID (parameterID);
        REQUIRE (parameter != nullptr);
        checkAgainstTracktion (s.song, id, *parameter);
    };

    // A track's fader, set to another volume and pan than a new one's.
    phazite::setChannelVolume (s.song, s.tracks[0], -6.0f);
    phazite::setChannelPan (s.song, s.tracks[0], -0.5f);
    check (phazite::getFader (s.song, s.tracks[0]), phazite::schema::faderVolume);
    check (phazite::getFader (s.song, s.tracks[0]), phazite::schema::faderPan);

    // The master's, at -3 dB as a new song's.
    check (phazite::getFader (s.song, {}), phazite::schema::masterVolume);
    check (phazite::getFader (s.song, {}), phazite::schema::masterPan);

    // A plain folder's VCA, which has no pan.
    const auto folder = phazite::insertFolder (s.song, s.tracks[1], nullptr);
    phazite::makePlainFolder (s.song, folder);
    REQUIRE_FALSE (phazite::isBus (s.song, folder));
    check (phazite::getFader (s.song, folder), phazite::schema::vcaVolume);
    CHECK_FALSE (
        phazite::describeParameter (s.song, ParameterRef { phazite::getFader (s.song, folder), phazite::schema::faderPan }).has_value());

    // A send to a return.
    const auto returnTrack = phazite::addNewReturnTrack (s.song, nullptr);
    const auto send = phazite::addSend (s.song, s.tracks[0], returnTrack, false);
    REQUIRE (send.isValid());
    check (send, phazite::schema::sendLevelParameter);
}

TEST_CASE ("A container's macros are described as Tracktion's, by their own IDs")
{
    Song s;
    const auto eq = phazite::insertDeviceState (s.song, s.tracks[0], phazite::makeOwnDeviceState (s.song, "phaziteEq"));
    const auto delay = phazite::insertDeviceState (s.song, s.tracks[0], phazite::makeOwnDeviceState (s.song, "phaziteDelay"));
    const auto container = phazite::groupIntoContainer (s.song, { eq, delay });
    REQUIRE (container.isValid());

    const auto macros = phazite::getContainerMacros (s.song, container);
    REQUIRE (macros.size() == (size_t) phazite::macrosPerContainer);

    for (const auto macro : macros)
    {
        const auto* rack = phazite::getContainerRack (s.tracktions (container));
        REQUIRE (rack != nullptr);
        te::MacroParameter* tracktions = nullptr;

        for (auto* each : rack->getMacroParameters())
            if (te::EditItemID::fromVar (each->paramID).getRawID() == macro.getRaw())
                tracktions = each;

        REQUIRE (tracktions != nullptr);
        const ParameterRef ref { phazite::DeviceID::fromRaw (macro.getRaw()), tracktions->paramID };
        const auto description = phazite::describeParameter (s.song, ref);
        REQUIRE (description.has_value());
        CHECK (description->name == tracktions->getParameterName());
        CHECK (description->range.start == doctest::Approx (tracktions->valueRange.start));
        CHECK (description->range.end == doctest::Approx (tracktions->valueRange.end));
        CHECK (phazite::getParameterText (s.song, ref, 0.25f) == tracktions->valueToString (0.25f));
        CHECK (phazite::getValueSetByHand (s.song, ref) == doctest::Approx (tracktions->getCurrentExplicitValue()));
    }
}

TEST_CASE ("A value set by hand is the song's, which Tracktion's parameter plays, and a curve's one point moves with it")
{
    Song s;
    const auto eq = phazite::insertDeviceState (s.song, s.tracks[0], phazite::makeOwnDeviceState (s.song, "phaziteEq"));
    auto& tracktions = s.tracktions (eq);
    auto* parameter = tracktions.getAutomatableParameters().getFirst();
    REQUIRE (parameter != nullptr);
    const ParameterRef ref { eq, parameter->paramID };
    const auto range = parameter->valueRange;

    // Set, clipped to its range.
    phazite::setValueSetByHand (s.song, ref, range.convertFrom0to1 (0.75f));
    CHECK (phazite::getValueSetByHand (s.song, ref) == doctest::Approx (range.convertFrom0to1 (0.75f)));
    CHECK (parameter->getCurrentExplicitValue() == doctest::Approx (range.convertFrom0to1 (0.75f)));
    phazite::setValueSetByHand (s.song, ref, range.end + 100.0f);
    CHECK (phazite::getValueSetByHand (s.song, ref) == doctest::Approx (range.end));

    // A fader's volume as its position, kept in decibels.
    const ParameterRef volume { phazite::getFader (s.song, s.tracks[0]), phazite::schema::faderVolume };
    phazite::setValueSetByHand (s.song, volume, phazite::engine::decibelsToFaderPosition (-12.0f));
    CHECK (phazite::getChannelVolume (s.song, s.tracks[0]) == doctest::Approx (-12.0f).epsilon (1.0e-3));

    // A curve with one point plays its value everywhere, so the point moves to the value.
    auto curves = s.song.getState().getOrCreateChildWithName (phazite::schema::AUTOMATION, nullptr);
    juce::ValueTree curve (phazite::schema::CURVE);
    curve.setProperty (phazite::schema::curveDevice, (juce::int64) eq.getRaw(), nullptr);
    curve.setProperty (phazite::schema::curveParameter, parameter->paramID, nullptr);
    curve.appendChild (
        juce::ValueTree (phazite::schema::POINT, { { phazite::schema::pointBeat, 4.0 }, { phazite::schema::pointValue, range.start } }),
        nullptr);
    curves.appendChild (curve, nullptr);
    phazite::setValueSetByHand (s.song, ref, range.convertFrom0to1 (0.5f));
    CHECK ((float) curve.getChild (0)[phazite::schema::pointValue] == doctest::Approx (range.convertFrom0to1 (0.5f)));
}

TEST_CASE ("Each modulator's parameters are described as Tracktion's modifier of its kind describes them, in their order")
{
    Song s;
    auto* track = phazite::mirror::findTrack (*s.edit, s.tracks[0]);
    REQUIRE (track != nullptr);

    for (const auto type : phazite::allModulatorTypes)
    {
        if (type == phazite::ModulatorType::macro)
            continue;

        const auto modulator = phazite::addModulator (*track, type);
        REQUIRE (modulator.has_value());
        REQUIRE (modulator->modifier != nullptr);
        INFO (phazite::getModulatorTypeName (type).toStdString());

        const auto id = phazite::DeviceID::fromRaw (modulator->getID().getRaw());
        const auto kind = phazite::getModulatorKind (phazite::findModulatorState (s.song, modulator->getID()));
        REQUIRE (kind.has_value());
        const auto infos = phazite::devices::getModulatorParameters (*kind);
        size_t place = 0;

        for (auto* parameter : modulator->modifier->getAutomatableParameters())
        {
            if (parameter->paramID == "enabled") // its switch is the song's deviceOff
                continue;

            INFO (parameter->paramID.toStdString());
            REQUIRE (place < infos.size());
            CHECK (juce::String (infos[place].id.data(), infos[place].id.size()) == parameter->paramID);
            ++place;

            const ParameterRef ref { id, parameter->paramID };
            const auto description = phazite::describeParameter (s.song, ref);
            REQUIRE (description.has_value());
            REQUIRE (description->info.has_value());
            CHECK (description->name == phazite::getModulatorParameterName (*parameter));
            CHECK (description->range.start == doctest::Approx (parameter->valueRange.start));
            CHECK (description->range.end == doctest::Approx (parameter->valueRange.end));
            CHECK (description->range.skew == doctest::Approx (parameter->valueRange.skew));
            REQUIRE (parameter->getDefaultValue().has_value());
            CHECK (description->defaultValue.value_or (-1.0f) == doctest::Approx (*parameter->getDefaultValue()));

            // Choices and switches move in whole numbers, which Tracktion's don't say.
            if (const auto unit = description->info->unit; unit != phazite::devices::Unit::choice && unit != phazite::devices::Unit::toggle)
            {
                CHECK (description->range.interval == doctest::Approx (parameter->valueRange.interval));

                for (const auto proportion : { 0.0f, 0.25f, 0.5f, 1.0f })
                {
                    const auto value = parameter->valueRange.convertFrom0to1 (proportion);
                    CHECK (phazite::getParameterText (s.song, ref, value) == parameter->valueToString (value));
                }
            }

            CHECK (phazite::getValueSetByHand (s.song, ref) == doctest::Approx (parameter->getCurrentExplicitValue()));
        }

        CHECK (place == infos.size());
    }
}
