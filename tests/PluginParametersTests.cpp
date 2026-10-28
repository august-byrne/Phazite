// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in's parameters through its updates (src/model/devices/internal/PluginParameters.h): a project saved with one
// version of the stand-in plug-in, opened with another that moved its parameters.
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DeviceStates.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"

namespace
{
using phazite::test::TestPlugin;

te::ExternalPlugin* findStandIn (te::Track& track)
{
    for (auto* device : track.pluginList.getPlugins())
        if (auto* plugin = dynamic_cast<te::ExternalPlugin*> (device))
            return plugin;

    return nullptr;
}

// The stand-in anywhere in the Edit, a container's rack included.
te::ExternalPlugin* findStandIn (te::Edit& edit)
{
    for (auto* device : te::getAllPlugins (edit, true))
        if (auto* plugin = dynamic_cast<te::ExternalPlugin*> (device))
            return plugin;

    return nullptr;
}

// A song with the stand-in on its track (in its first version: Gain "0", Tone "1"), Gain set by hand to 0.3, then
// automated from 0.2 to 0.8 and shown in a lane, and Tone moved by an LFO; saved as a project saves it.
juce::ValueTree makeSavedSong()
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto& track = *te::getAudioTracks (*edit)[0];
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (phazite::insertDevice (track, TestPlugin::create (*edit)));
    REQUIRE (plugin != nullptr);

    auto gain = plugin->getAutomatableParameterByID (TestPlugin::gainID);
    auto tone = plugin->getAutomatableParameterByID (TestPlugin::toneID);
    REQUIRE (gain != nullptr);
    REQUIRE (tone != nullptr);

    gain->setParameter (0.3f, juce::sendNotificationSync);
    phazite::addPoint (*gain, { phazite::BeatPosition::fromBeats (0.0), 0.2f });
    phazite::addPoint (*gain, { phazite::BeatPosition::fromBeats (4.0), 0.8f });
    gain->updateToFollowCurve (te::TimePosition());
    phazite::showLane (track, phazite::getParameterRef (*gain));

    const auto lfo = phazite::addModulator (track, phazite::ModulatorType::lfo);
    REQUIRE (lfo.has_value());
    phazite::setModulation (*lfo, *tone, 0.5f);

    phazite::storeDeviceStates (*edit);
    return edit->state.createCopy();
}

// The song opened again as a project opens, with the stand-in in the version set.
std::unique_ptr<te::Edit> open (const juce::ValueTree& saved)
{
    auto edit = phazite::createEditFromState (phazite::test::getEngine(), saved, {});
    REQUIRE (edit != nullptr);
    return edit;
}
} // namespace

TEST_CASE ("A project saves a plug-in's own IDs of the parameters it refers to")
{
    TestPlugin testPlugin;
    const auto saved = makeSavedSong();
    const auto plugin = saved.getChildWithName (te::IDs::TRACK).getChildWithProperty (te::IDs::type, te::ExternalPlugin::xmlTypeName);
    REQUIRE (plugin.isValid());

    // Gain, by its place "0", is the plug-in's "1"; Tone, "1", its "0". Nothing for the Drive of later versions.
    CHECK (plugin[phazite::schema::phaziteParameterIDs].toString() == "0=1 1=0");
}

TEST_CASE ("What a project refers to follows a plug-in's parameters that its update moved")
{
    TestPlugin testPlugin;
    const auto saved = makeSavedSong();

    // Version 2 has Tone first, then Gain, then a new Drive: "0" is now Tone and "1" Gain.
    testPlugin.setVersion (2);
    const auto edit = open (saved);
    auto& track = *te::getAudioTracks (*edit)[0];
    auto* plugin = findStandIn (track);
    REQUIRE (plugin != nullptr);
    const auto tone = plugin->getAutomatableParameterByID ("0");
    const auto gain = plugin->getAutomatableParameterByID ("1");
    const auto drive = plugin->getAutomatableParameterByID ("2");
    REQUIRE (tone != nullptr);
    REQUIRE (gain != nullptr);
    REQUIRE (drive != nullptr);
    REQUIRE (gain->getParameterName() == "Gain");
    REQUIRE (tone->getParameterName() == "Tone");

    // Gain keeps its automation, its value set by hand and its lane; Tone its LFO; Drive gets nothing.
    CHECK (phazite::getPoints (*gain).size() == 2);
    CHECK (gain->getCurrentExplicitValue() == doctest::Approx (0.3f));
    CHECK (phazite::getModulations (*gain).empty());

    CHECK (phazite::getPoints (*tone).empty());
    CHECK (tone->getCurrentExplicitValue() == doctest::Approx (0.5f));
    CHECK (phazite::getModulations (*tone).size() == 1);

    CHECK (phazite::getPoints (*drive).empty());
    CHECK (phazite::getModulations (*drive).empty());

    const auto lanes = phazite::getLanes (track);
    REQUIRE (lanes.size() == 1);
    CHECK (lanes[0].parameter == phazite::getParameterRef (*gain));

    // Saved again, by their new places.
    CHECK (plugin->state[phazite::schema::phaziteParameterIDs].toString() == "0=0 1=1");
}

TEST_CASE ("What a project refers to lets go of a parameter that a plug-in's update removed")
{
    TestPlugin testPlugin;
    const auto saved = makeSavedSong();

    // Version 3 has Tone alone: "0" is now Tone, and Gain is gone.
    testPlugin.setVersion (3);
    const auto edit = open (saved);
    auto& track = *te::getAudioTracks (*edit)[0];
    auto* plugin = findStandIn (track);
    REQUIRE (plugin != nullptr);
    const auto tone = plugin->getAutomatableParameterByID ("0");
    REQUIRE (tone != nullptr);
    REQUIRE (tone->getParameterName() == "Tone");

    // Gain's automation, value set by hand and lane don't land on Tone, which keeps its LFO.
    CHECK (phazite::getPoints (*tone).empty());
    CHECK (tone->getCurrentExplicitValue() == doctest::Approx (0.5f));
    CHECK (phazite::getModulations (*tone).size() == 1);
    CHECK (phazite::getLanes (track).empty());
    CHECK_FALSE (plugin->state.getChildWithName (te::IDs::AUTOMATIONCURVE).isValid());
    CHECK (plugin->state[phazite::schema::phaziteParameterIDs].toString() == "0=0");
}

TEST_CASE ("What a project refers to follows the parameters of a plug-in in a container too")
{
    TestPlugin testPlugin;
    juce::ValueTree saved;

    // Its lane is on the container's track, not with the plug-in in the container's rack.
    {
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        auto& track = *te::getAudioTracks (*edit)[0];
        REQUIRE (phazite::groupIntoContainer ({ phazite::insertDevice (track, TestPlugin::create (*edit)) }) != nullptr);
        auto* plugin = findStandIn (*edit);
        REQUIRE (plugin != nullptr);
        auto gain = plugin->getAutomatableParameterByID (TestPlugin::gainID);
        REQUIRE (gain != nullptr);
        phazite::addPoint (*gain, { phazite::BeatPosition::fromBeats (0.0), 0.2f });
        phazite::addPoint (*gain, { phazite::BeatPosition::fromBeats (4.0), 0.8f });
        phazite::showLane (track, phazite::getParameterRef (*gain));
        phazite::storeDeviceStates (*edit);
        saved = edit->state.createCopy();
    }

    testPlugin.setVersion (2);
    const auto edit = open (saved);
    auto* plugin = findStandIn (*edit);
    REQUIRE (plugin != nullptr);
    const auto gain = plugin->getAutomatableParameterByID ("1");
    REQUIRE (gain != nullptr);
    REQUIRE (gain->getParameterName() == "Gain");
    CHECK (phazite::getPoints (*gain).size() == 2);

    const auto lanes = phazite::getLanes (*te::getAudioTracks (*edit)[0]);
    REQUIRE (lanes.size() == 1);
    CHECK (lanes[0].parameter == phazite::getParameterRef (*gain));
}

TEST_CASE ("A project opened with the plug-in it was saved with keeps what it refers to as it was")
{
    TestPlugin testPlugin;
    const auto saved = makeSavedSong();
    const auto edit = open (saved);
    auto& track = *te::getAudioTracks (*edit)[0];
    auto* plugin = findStandIn (track);
    REQUIRE (plugin != nullptr);

    const auto gain = plugin->getAutomatableParameterByID (TestPlugin::gainID);
    const auto tone = plugin->getAutomatableParameterByID (TestPlugin::toneID);
    REQUIRE (gain != nullptr);
    REQUIRE (tone != nullptr);
    CHECK (phazite::getPoints (*gain).size() == 2);
    CHECK (gain->getCurrentExplicitValue() == doctest::Approx (0.3f));
    CHECK (phazite::getModulations (*tone).size() == 1);
    CHECK (phazite::getLanes (track).size() == 1);
    CHECK (plugin->state[phazite::schema::phaziteParameterIDs].toString() == "0=1 1=0");
}
