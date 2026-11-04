// SPDX-License-Identifier: AGPL-3.0-or-later
// How editors reach Phazite's own devices (src/model/devices/DeviceHandle.h), the knobs a device gets until it has an
// editor of its own (src/ui/devices/DeviceEditors.h), what every device's editor repaints, and the curves their graphs draw
// (src/ui/devices/DeviceGraph.h).
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "UiTestHelpers.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/DisplayTicker.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/DeviceGraph.h"

namespace
{
using phazite::devices::Equalizer;

// A device of a type at the end of a song's first track's devices (in place of its instrument, for an instrument).
phazite::DeviceID addDevice (phazite::Song& song, const char* type)
{
    const auto id = phazite::insertDeviceState (song, phazite::getTrackIDs (song)[0], phazite::makeOwnDeviceState (song, type));
    REQUIRE (id.isValid());
    return id;
}

// A song outside a project with a device of a type on its track, open in a context of its own, whose session plays it.
struct SongWithDevice
{
    explicit SongWithDevice (const char* type)
    {
        phazite::setSong (context, song.get());
        id = addDevice (*song, type);
        phazite::getSession (context)->update();
    }

    ~SongWithDevice() { phazite::setSong (context, nullptr); }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::DeviceID id;
};
} // namespace

TEST_CASE ("A device handle reaches one of Phazite's devices, and nothing else")
{
    phazite::test::TestPlugin testPlugin;
    auto song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::setSong (context, song.get());
    const auto eq = addDevice (*song, Equalizer::typeID);
    phazite::getSession (context)->update();
    const auto device = phazite::makeDeviceHandle (*song, eq);

    REQUIRE (device.isValid());
    REQUIRE (device.getInfo() != nullptr);
    CHECK (device.getInfo()->name == "EQ");
    CHECK (device.getParameter ("band3Gain").getInfo()->unit == phazite::devices::Unit::decibels);
    CHECK_FALSE (device.getParameter ("volume").isValid());
    CHECK (device.getProcessor<Equalizer>() != nullptr);
    CHECK (device.getProcessor<phazite::devices::Processor>() != nullptr);

    // A plug-in isn't one of Phazite's devices.
    const auto plugin = phazite::test::TestPlugin::add (*song, phazite::getTrackIDs (*song)[0]);
    const auto notOurs = phazite::makeDeviceHandle (*song, plugin);
    CHECK_FALSE (notOurs.isValid());
    CHECK (notOurs.getInfo() == nullptr);
    CHECK (notOurs.getProcessor<Equalizer>() == nullptr);

    // When the project closes, the handle has nothing.
    phazite::setSong (context, nullptr);
    song = nullptr;
    CHECK_FALSE (device.isValid());
    CHECK (device.getProcessor<Equalizer>() == nullptr);
    CHECK_FALSE (device.getParameter ("band3Gain").isValid());
}

TEST_CASE ("A device without an editor of its own gets a knob for each parameter, showing its unit")
{
    auto song = phazite::createSong();
    const auto eq = addDevice (*song, Equalizer::typeID);
    song->getUndoManager().clearUndoHistory();

    phazite::ui::DeviceKnobs knobs ((phazite::makeDeviceHandle (*song, eq)));
    REQUIRE (knobs.getNumKnobs() == 50);

    auto& frequency = knobs.getKnob (Equalizer::getParameterIndex (0, Equalizer::frequency));
    CHECK (frequency.getTitle() == juce::String ("Band 1 Frequency"));
    CHECK (frequency.getTextFromValue (frequency.getValue()) == juce::String ("30.0 Hz"));
    CHECK (frequency.getValueFromText ("1.2k") == doctest::Approx (1200.0));

    // Turning a knob changes its parameter, as an undo step the model names.
    auto& gain = knobs.getKnob (Equalizer::getParameterIndex (2, Equalizer::gain));
    gain.setValue (6.0, juce::sendNotificationSync);
    CHECK (phazite::getValueSetByHand (*song, phazite::ParameterRef { eq, "band3Gain" }) == doctest::Approx (6.0f));
    CHECK (phazite::describeUndo (song.get()) == juce::String ("Undo Change Band 3 Gain"));

    CHECK (phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (*song, eq)) != nullptr);
    CHECK (phazite::ui::createDeviceEditor (phazite::DeviceHandle()) == nullptr);
}

TEST_CASE ("Every device's editor rests while nothing it shows moves, and shows a value as it changes")
{
    // Their pictures once repainted at each tick of the displays' ticker, 30 times a second, with nothing playing and nothing
    // turned (docs/performance-audit.md, section 8).
    juce::SharedResourcePointer<phazite::ui::DisplayTicker> ticker;
    const auto tick = [&ticker] (int times)
    {
        for (int i = 0; i < times; ++i)
            ticker->tickNow();
    };

    for (const auto* info : phazite::getOwnDevices())
    {
        CAPTURE (std::string (info->name));
        SongWithDevice song (std::string (info->typeID).c_str());
        auto editor = phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (*song.song, song.id));
        REQUIRE (editor != nullptr);

        {
            phazite::test::TestWindow window (*editor, std::max (1200, editor->getWidth()), std::max (700, editor->getHeight()));

            // Long enough for what falls back to rest: a compressor's dot falls from its lowest level shown to silence.
            tick (200);
            window.takeRepaintedArea();
            tick (30);
            CHECK (window.takeRepaintedArea().isEmpty());

            // A value turned elsewhere (automation, say), other than the mix, which the header shows: its control at least
            // shows it.
            const auto shown = std::find_if (info->parameters.begin(), info->parameters.end(),
                                             [] (const auto& parameter) { return parameter.id != phazite::devices::mixParameterID; });
            REQUIRE (shown != info->parameters.end());
            const phazite::ParameterRef parameter { song.id, juce::String (shown->id.data(), shown->id.size()) };
            REQUIRE (phazite::describeParameter (*song.song, parameter).has_value());
            phazite::setValueSetByHand (*song.song, parameter,
                                        juce::approximatelyEqual (shown->defaultValue, shown->maximum) ? shown->minimum : shown->maximum);
            tick (1);
            CHECK_FALSE (window.takeRepaintedArea().isEmpty());
        }

        editor = nullptr; // before its song
    }
}

TEST_CASE ("A graph's curve is one line from end to end, through the level at each step")
{
    // Every point began a path of its own (Path::isEmpty() holds until a path has a line), so that no graph drew its line,
    // and a wash under one was a wedge from its last point (2026-11-01, the multiband's curve).
    const juce::Rectangle<float> area (10.0f, 20.0f, 100.0f, 50.0f);
    const auto curve = phazite::ui::graph::makeCurve (area, [] (float x) { return x < 60.0f ? 70.0f : 20.0f; }, 1.0f);
    juce::Path::Iterator element (curve);
    int starts = 0, lines = 0;

    while (element.next())
    {
        if (element.elementType == juce::Path::Iterator::startNewSubPath)
            ++starts;
        else if (element.elementType == juce::Path::Iterator::lineTo)
            ++lines;
    }

    CHECK (starts == 1);
    CHECK (lines == 100);
    CHECK (curve.getBounds() == area);

    // Under it, the area below the line: low to the left, high to the right.
    auto wash = curve;
    wash.lineTo (area.getBottomRight());
    wash.lineTo (area.getBottomLeft());
    wash.closeSubPath();
    CHECK (! wash.contains (30.0f, 60.0f));
    CHECK (wash.contains (90.0f, 30.0f));
}
