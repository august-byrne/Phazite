// SPDX-License-Identifier: AGPL-3.0-or-later
// The dynamics curves (src/devices/dynamics/DynamicsCurve.h) on their decibel axes, as the compressor and the multiband play
// them; their view (src/ui/devices/DynamicsCurve.h), which draws what they play and is worked by the mouse on it; and the
// dynamics history (src/ui/devices/LevelHistoryView.h) with the compressor's trail of readings (devices/core/LevelTrail.h),
// which draws a ramp as a line, not as stairs.
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dsp/Math.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/CompressorEditor.h"
#include "ui/devices/DeviceTint.h"
#include "ui/devices/LevelHistoryView.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/theme/Popups.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace
{
using phazite::devices::Compressor;
namespace curves = phazite::devices::curves;
namespace dsp = phazite::devices::dsp;

// A curve's points as its parameters hold them: the count, then each point, the last repeated in the places it leaves.
std::array<float, curves::parameterCount> toValues (const std::vector<curves::Point>& points)
{
    std::array<float, curves::parameterCount> values {};
    values[0] = (float) points.size();

    for (int i = 0; i < curves::maxPoints; ++i)
        for (int field = 0; field < curves::pointFields; ++field)
            values[(size_t) curves::getPointParameter (i, (curves::PointField) field)] =
                points[(size_t) std::min (i, (int) points.size() - 1)][(size_t) field];

    return values;
}

curves::Point makePoint (float across, float up, curves::PointType type = curves::PointType::singleCurve, float tension = 0.0f)
{
    return { across, up, tension, (float) type };
}

// How many decibels a compressor with this curve turns a level up (below 0, down), once settled.
float getGain (const std::vector<curves::Point>& points, float decibels)
{
    curves::Gains gains;
    gains.update (toValues (points));
    return gains.getDecibels (dsp::decibelsToGain (decibels));
}

constexpr std::array allTypes { curves::PointType::singleCurve, curves::PointType::doubleCurve, curves::PointType::hold,
                                curves::PointType::stairs, curves::PointType::smoothStairs };

// The points of a path drawn of straight lines.
std::vector<juce::Point<float>> getPathPoints (const juce::Path& path)
{
    std::vector<juce::Point<float>> points;

    for (juce::Path::Iterator it (path); it.next();)
        points.push_back ({ it.x1, it.y1 });

    return points;
}

// A menu open inside a window (as the app's open: ui/theme/Popups.h), or nullptr.
juce::Component* findMenuWindow (const juce::Component& window)
{
    for (auto* child : window.getChildren())
        if (child->getName() == "menu")
            return child;

    return nullptr;
}

// A track with a compressor on it, open in a context of its own whose session plays it.
struct Song
{
    Song()
    {
        phazite::setSong (context, song.get());
        const auto tracks = phazite::ensureTracks (*song, 1);
        compressor = phazite::test::addOwnDevice (*song, tracks[0], Compressor::typeID);
        REQUIRE (compressor.isValid());
        phazite::getSession (context)->update();
    }

    ~Song() { phazite::setSong (context, nullptr); }

    phazite::DeviceHandle getDevice() const { return phazite::makeDeviceHandle (*song, compressor); }
    float valueOf (const char* id) const { return phazite::getValueSetByHand (*song, phazite::ParameterRef { compressor, id }); }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::DeviceID compressor;
};

// A compressor's curve set to these points, each value by hand.
void setPoints (const phazite::DeviceHandle& device, const std::vector<curves::Point>& points)
{
    auto count = device.getParameter ("points");
    count.setValue ((float) points.size());

    for (int point = 0; point < curves::maxPoints; ++point)
        for (int field = 0; field < curves::pointFields; ++field)
        {
            const auto index = Compressor::points + curves::getPointParameter (point, (curves::PointField) field);
            auto handle = device.getParameter (Compressor::info.parameters[(size_t) index].id);
            handle.setValue (points[(size_t) std::min (point, (int) points.size() - 1)][(size_t) field]);
        }
}

// Where the view shows a place across and up, from its corners: what the device plays, drawn on the same even axes.
juce::Point<float> getExpectedPosition (const phazite::ui::DynamicsCurve& curve, float across, float up)
{
    const auto bottomLeft = curve.getPosition (0.0f, 0.0f), topRight = curve.getPosition (1.0f, 1.0f);
    return { bottomLeft.x + across * (topRight.x - bottomLeft.x), bottomLeft.y + up * (topRight.y - bottomLeft.y) };
}

float getAcross (const phazite::ui::DynamicsCurve& curve, float x)
{
    const auto bottomLeft = curve.getPosition (0.0f, 0.0f), topRight = curve.getPosition (1.0f, 1.0f);
    return (x - bottomLeft.x) / (topRight.x - bottomLeft.x);
}
} // namespace

TEST_CASE ("The dynamics curves' axes are decibels, the same across and up, from -60 dB to the top; out at the bottom is silence")
{
    CHECK (curves::topDecibels == doctest::Approx (20.0 * std::log10 (4.0)).epsilon (1.0e-6)); // where the old axes ended
    CHECK (curves::axisForDecibels (-60.0f) == 0.0f);
    CHECK (curves::axisForDecibels (-90.0f) == 0.0f);
    CHECK (curves::axisForDecibels (curves::topDecibels) == doctest::Approx (1.0f));
    CHECK (curves::axisForDecibels (0.0f) == doctest::Approx (60.0f / (60.0f + curves::topDecibels)));
    CHECK (curves::axisForDecibels (-24.0f) - curves::axisForDecibels (-30.0f)
           == doctest::Approx (curves::axisForDecibels (6.0f) - curves::axisForDecibels (0.0f))); // evenly
    CHECK (curves::decibelsForAxis (0.5f) == doctest::Approx (-60.0f + 0.5f * (60.0f + curves::topDecibels)));
    CHECK (curves::toAxis (dsp::decibelsToGain (-24.0f)) == doctest::Approx (curves::axisForDecibels (-24.0f)));
    CHECK (curves::toAxis (0.0f) == 0.0f);
    CHECK (curves::fromAxis (curves::axisForDecibels (-24.0f)) == doctest::Approx (dsp::decibelsToGain (-24.0f)));
    CHECK (curves::fromAxis (0.0f) == 0.0f); // out at the bottom: silence
}

TEST_CASE ("A line with no tension is a ratio, and a bent one stays in the box its points span, at any tension and of any type")
{
    for (const auto type : allTypes)
    {
        for (const auto& [low, high] : { std::pair { 0.3f, 0.9f }, std::pair { 0.9f, 0.3f } })
        {
            for (const auto tension : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
            {
                CAPTURE ((int) type);
                CAPTURE (low);
                CAPTURE (tension);
                const auto values = toValues (
                    { makePoint (0.0f, low), makePoint (0.2f, low), makePoint (0.7f, high, type, tension), makePoint (1.0f, high) });
                auto before = low;

                for (int k = 0; k <= 200; ++k)
                {
                    const auto place = 0.2f + 0.5f * (float) k / 200.0f;
                    const auto at = curves::getAt (values, place);
                    CHECK (at >= std::min (low, high) - 1.0e-6f);
                    CHECK (at <= std::max (low, high) + 1.0e-6f);
                    CHECK ((at - before) * (high - low) >= -1.0e-6f); // never back the other way
                    before = at;

                    if (type == curves::PointType::singleCurve && juce::exactlyEqual (tension, 0.0f))
                        CHECK (at == doctest::Approx (low + (high - low) * (place - 0.2f) / 0.5f).epsilon (1.0e-5));
                }
            }
        }
    }
}

TEST_CASE ("A compressor's straight line is its ratio, and the levels under -60 dB come out with the gain just over it")
{
    // 4:1 over -18 dB: exact, along its straight line, as the compressor plays it.
    const auto fourToOne = curves::makeCompression (-18.0f, 4.0f);
    CHECK (fourToOne.size() == 3);

    for (const auto in : { -40.0f, -20.0f, -16.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
    {
        CAPTURE (in);
        CHECK (getGain (fourToOne, in) == doctest::Approx (in <= -18.0f ? 0.0f : -0.75f * (in + 18.0f)).epsilon (0.002));
    }

    // Under -60 dB: unchanged under a line from the corner; silent under a gate's floor; under Lift's, 15 dB louder, as at
    // -60 dB; 1:2 under -24 dB down to silence, reached at -42 dB.
    for (const auto in : { -70.0f, -90.0f, -110.0f })
    {
        CAPTURE (in);
        CHECK (getGain (curves::findShape ("No Compression")->points, in) == doctest::Approx (0.0f).epsilon (0.001));
        CHECK (getGain (fourToOne, in) == doctest::Approx (0.0f).epsilon (0.001));
        CHECK (getGain (curves::findShape ("Gate under -40 dB")->points, in) < -100.0f);
        CHECK (getGain (curves::findShape ("Lift under -30 dB")->points, in) == doctest::Approx (15.0f).epsilon (0.002));
    }

    CHECK (getGain (curves::findShape ("Lift under -30 dB")->points, -45.0f) == doctest::Approx (7.5f).epsilon (0.002));
    CHECK (getGain (curves::findShape ("Gate under -40 dB")->points, -45.0f) < -100.0f);
    CHECK (getGain (curves::findShape ("Gate under -40 dB")->points, -30.0f) == doctest::Approx (0.0f).epsilon (0.002));
    CHECK (getGain (curves::findShape ("Expand under -24 dB")->points, -33.0f) == doctest::Approx (-9.0f).epsilon (0.002));
    CHECK (getGain (curves::findShape ("Expand under -24 dB")->points, -50.0f) < -100.0f);
    CHECK (getGain (curves::findShape ("Limit at 0 dB")->points, 9.0f) == doctest::Approx (-9.0f).epsilon (0.002));
}

TEST_CASE ("What the dynamics curve draws is what the compressor plays: its line, its points and its handles, for each line's shape")
{
    Song song;
    const auto device = song.getDevice();
    phazite::ui::CompressorEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& curve = editor.getCurve();

    for (const auto type : allTypes)
    {
        for (const auto tension : { -0.7f, 0.0f, 0.7f })
        {
            CAPTURE ((int) type);
            CAPTURE (tension);
            setPoints (device, { makePoint (0.0f, 0.0f), makePoint (curves::axisForDecibels (-40.0f), curves::axisForDecibels (-40.0f)),
                                 makePoint (curves::axisForDecibels (-10.0f), curves::axisForDecibels (-20.0f), type, tension),
                                 makePoint (1.0f, curves::axisForDecibels (-12.0f), type, -tension) });
            editor.refresh();
            const auto values = toValues (curve.getPoints());
            REQUIRE (curve.getCount() == 4);

            // Each of its points where its levels are.
            for (int i = 0; i < 4; ++i)
            {
                const auto expected = getExpectedPosition (curve, values[(size_t) curves::getPointParameter (i, curves::pointX)],
                                                           values[(size_t) curves::getPointParameter (i, curves::pointY)]);
                CHECK (curve.getPointPosition (i).getDistanceFrom (expected) < 0.01f);
            }

            // The line through every pixel across where the curve plays it; at a step (a hold's, stairs'), either side of it.
            const auto onCurve = [&] (juce::Point<float> at)
            {
                const auto across = getAcross (curve, at.x);

                for (const auto nudge : { 0.0f, -1.0e-3f, 1.0e-3f })
                    if (std::abs (at.y - getExpectedPosition (curve, across, curves::getAt (values, across + nudge)).y) < 0.5f)
                        return true;

                return false;
            };

            const auto points = getPathPoints (curve.getLine());
            CHECK (points.size() >= (size_t) (curve.getPosition (1.0f, 0.0f).x - curve.getPosition (0.0f, 0.0f).x));
            auto off = 0;

            for (const auto& at : points)
                off += onCurve (at) ? 0 : 1;

            CHECK (off == 0);

            // Each handle on its line, half way across it.
            for (int i = 1; i < 4; ++i)
            {
                const auto handle = curve.getTensionPosition (i);
                REQUIRE (handle.has_value());
                CHECK (onCurve (*handle));
                CHECK (handle->x == doctest::Approx (0.5f * (curve.getPointPosition (i - 1).x + curve.getPointPosition (i).x)));
            }
        }
    }

    // The compressor's own curve at first, drawn where its gains put each level out.
    setPoints (device, curves::makeCompression (-18.0f, 4.0f));
    editor.refresh();

    for (const auto& at : getPathPoints (curve.getLine()))
    {
        const auto in = curves::decibelsForAxis (getAcross (curve, at.x));
        const auto out = in + getGain (curve.getPoints(), in);
        CHECK (std::abs (at.y - getExpectedPosition (curve, 0.0f, curves::axisForDecibels (out)).y) < 0.5f);
    }
}

TEST_CASE ("A right-click on the curve's handle straightens its line where it shows, the handle in its middle; a drag bends it in its box")
{
    Song song;
    const auto device = song.getDevice();
    phazite::ui::CompressorEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& curve = editor.getCurve();
    setPoints (device,
               { makePoint (0.0f, 0.0f), makePoint (curves::axisForDecibels (-40.0f), curves::axisForDecibels (-40.0f)),
                 makePoint (curves::axisForDecibels (-6.0f), curves::axisForDecibels (-24.0f), curves::PointType::singleCurve, 0.6f),
                 makePoint (1.0f, curves::axisForDecibels (-18.0f)) });
    editor.refresh();
    phazite::test::Mouse mouse (curve);

    // The line's points between its two ends, and where the straight line between them is at each.
    const auto between = [&curve]
    {
        const auto from = curve.getPointPosition (1), to = curve.getPointPosition (2);
        std::vector<std::pair<juce::Point<float>, float>> shown;

        for (const auto& at : getPathPoints (curve.getLine()))
            if (at.x > from.x + 0.5f && at.x < to.x - 0.5f)
                shown.push_back ({ at, from.y + (to.y - from.y) * (at.x - from.x) / (to.x - from.x) });

        return shown;
    };

    const auto inBox = [&curve] (juce::Point<float> at)
    {
        const auto from = curve.getPointPosition (1), to = curve.getPointPosition (2);
        return at.y >= std::min (from.y, to.y) - 0.5f && at.y <= std::max (from.y, to.y) + 0.5f;
    };

    // Bent at first, its line above the straight one between its ends.
    const auto bent = between();
    REQUIRE (! bent.empty());
    CHECK (std::any_of (bent.begin(), bent.end(), [] (const auto& shown) { return shown.first.y < shown.second - 3.0f; }));

    // Straightened: on the straight line, the handle at its middle.
    const auto handle = [&curve] { return *curve.getTensionPosition (2); };
    mouse.click (handle().roundToInt(), juce::ModifierKeys::rightButtonModifier);
    editor.refresh();
    CHECK (song.valueOf ("point3Tension") == 0.0f);

    for (const auto& [at, straight] : between())
        CHECK (std::abs (at.y - straight) < 0.5f);

    CHECK (handle().getDistanceFrom ((curve.getPointPosition (1) + curve.getPointPosition (2)) / 2.0f) < 0.5f);

    // Dragged up, it bends up, toward the pointer; dragged down, down; within its ends' box either way.
    for (const auto up : { true, false })
    {
        CAPTURE (up);
        const auto from = handle().roundToInt();
        mouse.press (from);
        mouse.dragTo (from + juce::Point<int> (0, up ? -60 : 120));
        mouse.release();
        editor.refresh();
        CHECK ((up ? song.valueOf ("point3Tension") > 0.2f : song.valueOf ("point3Tension") < -0.2f));
        auto furthest = 0.0f;

        for (const auto& [at, straight] : between())
        {
            CHECK (inBox (at));
            CHECK ((up ? at.y <= straight + 0.5f : at.y >= straight - 0.5f));
            furthest = std::max (furthest, std::abs (at.y - straight));
        }

        CHECK (furthest > 3.0f);
        CHECK (inBox (handle()));
    }
}

TEST_CASE ("A right-click on a curve's point opens its menu at the click, not under the graph")
{
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    const phazite::test::ScopedDisplay display; // JUCE places a menu on its display

    Song song;
    phazite::ui::CompressorEditor editor (song.getDevice());
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    phazite::ui::setPopupsInside (window);
    auto& curve = editor.getCurve();
    const auto point = curve.getPointPosition (1).roundToInt();
    phazite::test::Mouse mouse (curve);
    mouse.click (point, juce::ModifierKeys::rightButtonModifier);

    // JUCE's menu takes itself down on a timer where the program isn't the foreground one (Linux CI).
    if (auto* menu = findMenuWindow (window))
        CHECK (menu->getBounds().expanded (4).contains (window.getLocalPoint (&curve, point)));
    else
        CHECK_FALSE (juce::Process::isForegroundProcess());

    juce::PopupMenu::dismissAllActiveMenus();
    phazite::test::dispatchPendingMessages();
}

TEST_CASE ("The compressor's trail gives a reading every half millisecond while a display listens, its sound mixed as it is heard")
{
    Compressor compressor;
    compressor.prepare (48000.0, 4800);
    std::vector<float> values;

    for (const auto& parameter : Compressor::info.parameters)
        values.push_back (parameter.defaultValue);

    values[Compressor::attack] = 0.1f;
    juce::AudioBuffer<float> buffer (2, 4800), dry (2, 4800);
    const auto fill = [&buffer, &dry]
    {
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            for (int channel = 0; channel < 2; ++channel)
            {
                buffer.setSample (channel, i, (i % 2 == 0 ? 1.0f : -1.0f) * dsp::decibelsToGain (-6.0f));
                dry.setSample (channel, i, buffer.getSample (channel, i));
            }
    };

    const auto play = [&]
    {
        fill();
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        float* dryChannels[] = { dry.getWritePointer (0), dry.getWritePointer (1) };
        phazite::devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = buffer.getNumSamples();
        block.parameters = values;
        block.dry = dryChannels;
        compressor.process (block);
    };

    // Nobody listening: nothing.
    std::vector<phazite::devices::LevelReading> readings ((size_t) phazite::devices::LevelTrail::capacity);
    play();
    CHECK (compressor.getTrail().pull (readings.data(), (int) readings.size()) == 0);

    // Listening: 100 ms, 200 readings, each what came in, how far 4:1 over -18 dB turned -6 dB down once settled, the level
    // heard, and what went out.
    compressor.getTrail().setListening (true);
    play();
    play();
    REQUIRE (compressor.getTrail().pull (readings.data(), (int) readings.size()) == 400);
    const auto& last = readings[399];
    CHECK (dsp::gainToDecibels (last.input) == doctest::Approx (-6.0f).epsilon (0.001));
    CHECK (dsp::gainToDecibels (last.heard) == doctest::Approx (-6.0f).epsilon (0.01));
    CHECK (last.down == doctest::Approx (9.0f).epsilon (0.01));
    CHECK (last.up == 0.0f);
    CHECK (dsp::gainToDecibels (last.output) == doctest::Approx (-15.0f).epsilon (0.01));

    // At half mix, what went out is half the turned sound and half what came in, as its runner mixes them.
    values[Compressor::mix] = 0.5f;
    play();
    REQUIRE (compressor.getTrail().pull (readings.data(), (int) readings.size()) == 200);
    CHECK (readings[199].output == doctest::Approx (0.5f * (dsp::decibelsToGain (-15.0f) + dsp::decibelsToGain (-6.0f))).epsilon (0.01));

    // Drained, nothing left; no longer listening, nothing taken.
    play();
    compressor.getTrail().drain();
    CHECK (compressor.getTrail().pull (readings.data(), (int) readings.size()) == 0);
    compressor.getTrail().setListening (false);
    play();
    CHECK (compressor.getTrail().pull (readings.data(), (int) readings.size()) == 0);
}

TEST_CASE ("A dynamics history draws a duck's release as a line, not as stairs, its readings fine or one a refresh")
{
    // A pad at -12 dB, ducked 12 dB every two seconds, at once, held 100 ms and let go over 500 ms, straight in decibels; the
    // key at -50 dB. Pushed at 30 refreshes a second, each with the trail's readings since the last (one every half
    // millisecond) or the loudest since the last alone, at 120 BPM, two bars across.
    const auto duck = [] (double ms)
    {
        const auto since = std::fmod (ms, 2000.0);
        return (float) (since < 100.0 ? 12.0 : since < 600.0 ? 12.0 * (1.0 - (since - 100.0) / 500.0) : 0.0);
    };

    for (const auto fine : { true, false })
    {
        for (const auto width : { 400, 800 })
        {
            CAPTURE (fine);
            CAPTURE (width);
            phazite::ui::LevelHistoryView history ("History", -60.0f, 24.0f);
            history.setKey ("Kick", juce::Colours::magenta);
            phazite::test::TestWindow window (history, width, 160);
            constexpr double tick = 1000.0 / 30.0;
            double last = 0.0;
            phazite::ui::LevelHistoryView::SongTime song;
            song.playing = true;

            for (auto now = tick; now < 7500.0; now += tick)
            {
                std::vector<phazite::ui::LevelHistoryView::Levels> levels;

                if (fine)
                    for (auto ms = last + 0.5; ms <= now + 1.0e-9; ms += 0.5)
                        levels.push_back ({ -12.0f, -12.0f - duck (ms - 0.25), duck (ms - 0.25), -50.0f });
                else
                {
                    auto least = 100.0f;

                    for (auto ms = last; ms <= now; ms += 0.5)
                        least = std::min (least, duck (ms));

                    levels.push_back ({ -12.0f, -12.0f - least, least, -50.0f });
                }

                song.beat = now / 500.0;
                song.barStart = 4.0 * std::floor (song.beat / 4.0);
                history.push (now, song, levels);
                last = now;
            }

            // The release in view, from 6100 ms to 6600 ms, column by column: the row nearest the device's colour, what went
            // out's line, moves a pixel at most from one to the next (drawn as steps as wide as a refresh, as before, it would
            // climb about 2 pixels at each).
            juce::Image image (juce::Image::ARGB, width, 160, true, juce::SoftwareImageType());
            {
                juce::Graphics g (image);
                history.paintEntireComponent (g, false);
            }

            const auto tint = phazite::ui::getDeviceTint (history);
            const auto distance = [tint] (juce::Colour c)
            {
                return std::abs (c.getFloatRed() - tint.getFloatRed()) + std::abs (c.getFloatGreen() - tint.getFloatGreen())
                       + std::abs (c.getFloatBlue() - tint.getFloatBlue());
            };

            // (Its first and last 50 ms left out: the corner at the bottom, and the line meeting what came in at the top.)
            const auto from = juce::roundToInt (history.getXForTime (6150.0)), to = juce::roundToInt (history.getXForTime (6550.0));
            const auto top = juce::roundToInt (history.getYForLevel (-11.0f)), bottom = juce::roundToInt (history.getYForLevel (-25.0f));
            REQUIRE (to - from > 35);
            auto lastRow = -1, jumps = 0;

            for (auto x = from; x <= to; ++x)
            {
                auto row = top;

                for (auto y = top; y <= bottom; ++y)
                    if (distance (image.getPixelAt (x, y)) < distance (image.getPixelAt (x, row)))
                        row = y;

                jumps += lastRow >= 0 && std::abs (row - lastRow) > 1 ? 1 : 0;
                lastRow = row;
            }

            CHECK (jumps == 0);
        }
    }
}
