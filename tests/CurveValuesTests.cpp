// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's automation as values (model/base/internal/CurveValues.h; roadmap step S5.20, automation's first piece): the
// conversion between the values and the AUTOMATION element (the one place that reads and writes CURVE and POINT by name),
// round trip through every curve of the golden files and through damaged ones, in their order; the values' helpers; and what
// reads the song's curves (model/automation/internal/Automation.h: getCurveKeys(), getCurveValue(), findCurve()...),
// the same as what the tree held, read as automation's code read it before the values came.
//   phazite_tests.exe --test-case="Curve values*"
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "engine/automation/Curve.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Song.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
#include "model/project/internal/Project.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::CurveValue;
using phazite::ParameterRef;
using phazite::SongCurves;

juce::ValueTree parse (const juce::String& xml)
{
    const auto parsed = juce::parseXML (xml);
    REQUIRE (parsed != nullptr);
    return juce::ValueTree::fromXml (*parsed);
}

// An AUTOMATION as XML, each attribute that is a number written as the number (a file's "0.5" and the value's 0.5 alike):
// a point's value and curve as the float a point holds, the rest as doubles. For findDifferences() (GoldenFiles.h), which
// compares text.
std::unique_ptr<juce::XmlElement> toComparable (const juce::ValueTree& automation)
{
    auto xml = automation.createXml();
    REQUIRE (xml != nullptr);

    const std::function<void (juce::XmlElement&)> normalise = [&normalise] (juce::XmlElement& element)
    {
        for (int i = 0; i < element.getNumAttributes(); ++i)
        {
            const auto name = element.getAttributeName (i);
            const auto text = element.getAttributeValue (i);

            if (text.isEmpty() || ! text.containsOnly ("0123456789.-+eE") || ! text.containsAnyOf ("0123456789"))
                continue;

            const auto isFloat = element.hasTagName (schema::POINT.toString())
                                 && (name == schema::pointValue.toString() || name == schema::pointCurve.toString());
            element.setAttribute (name,
                                  isFloat ? juce::String ((double) (float) text.getDoubleValue()) : juce::String (text.getDoubleValue()));
        }

        for (auto* child : element.getChildIterator())
            normalise (*child);
    };

    normalise (*xml);
    return xml;
}

juce::StringArray findCurveDifferences (const juce::ValueTree& expected, const juce::ValueTree& actual)
{
    return phazite::test::findDifferences (*toComparable (expected), *toComparable (actual));
}

// The song's curves written from their value, into an empty AUTOMATION.
juce::ValueTree writeAll (const SongCurves& curves)
{
    juce::ValueTree written (schema::AUTOMATION);
    phazite::writeCurves (curves, written);
    return written;
}

// The parameters of an AUTOMATION's curves, in its order.
std::vector<ParameterRef> getParameters (const juce::ValueTree& automation)
{
    std::vector<ParameterRef> parameters;

    for (const auto& child : automation)
        if (child.hasType (schema::CURVE))
            parameters.push_back (phazite::readCurveParameter (child));

    return parameters;
}

std::vector<ParameterRef> toVector (const immer::flex_vector<ParameterRef>& order)
{
    return { order.begin(), order.end() };
}

// A curve's points as automation's code read them from its element before the values came (Automation.cpp, getPoints() at
// commit f177adcd): every child, its beat, value and curve (0 where it has none); and as Phazite's engine played them
// (SongPlayer.cpp, readCurves() then): its POINT children alone.
std::vector<phazite::AutomationPoint> readPointsAsBefore (const juce::ValueTree& curve)
{
    std::vector<phazite::AutomationPoint> points;

    for (const auto& point : curve)
        points.push_back ({ phazite::BeatPosition::fromBeats (point[schema::pointBeat]), point[schema::pointValue],
                            point.getProperty (schema::pointCurve, 0.0f) });

    return points;
}

std::vector<phazite::engine::CurvePoint> readPlayedAsBefore (const juce::ValueTree& curve)
{
    std::vector<phazite::engine::CurvePoint> points;

    for (const auto& point : curve)
        if (point.hasType (schema::POINT))
            points.push_back ({ (double) point[schema::pointBeat], (float) point[schema::pointValue],
                                (float) point.getProperty (schema::pointCurve, 0.0f) });

    return points;
}

bool isSame (const std::vector<phazite::AutomationPoint>& a, const std::vector<phazite::AutomationPoint>& b)
{
    return std::equal (a.begin(), a.end(), b.begin(), b.end(),
                       [] (const phazite::AutomationPoint& x, const phazite::AutomationPoint& y)
                       {
                           return juce::exactlyEqual (x.beat.inBeats(), y.beat.inBeats()) && juce::exactlyEqual (x.position, y.position)
                                  && juce::exactlyEqual (x.curve, y.curve);
                       });
}

std::vector<phazite::engine::CurvePoint> toEnginePoints (const CurveValue& curve)
{
    std::vector<phazite::engine::CurvePoint> points;

    for (const auto& point : curve.points)
        points.push_back ({ point.beat.inBeats(), point.position, point.curve });

    return points;
}

bool isSame (const std::vector<phazite::engine::CurvePoint>& a, const std::vector<phazite::engine::CurvePoint>& b)
{
    return std::equal (a.begin(), a.end(), b.begin(), b.end(),
                       [] (const phazite::engine::CurvePoint& x, const phazite::engine::CurvePoint& y)
                       {
                           return juce::exactlyEqual (x.beat, y.beat) && juce::exactlyEqual (x.position, y.position)
                                  && juce::exactlyEqual (x.curve, y.curve);
                       });
}

// The golden files that hold automation, each AUTOMATION as its file holds it.
std::vector<std::pair<juce::String, juce::ValueTree>> readGoldenCurves()
{
    std::vector<std::pair<juce::String, juce::ValueTree>> found;

    for (const auto* name : { "projects/representative.xml", "commands/script.xml" })
    {
        const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile (name));
        REQUIRE (xml != nullptr);
        const auto* automation = xml->getChildByName (schema::AUTOMATION.toString());
        REQUIRE (automation != nullptr);
        found.emplace_back (name, juce::ValueTree::fromXml (*automation));
    }

    return found;
}

// A golden project opened as a project from a file is.
std::unique_ptr<phazite::Project> openGolden (const phazite::test::TemporaryFolder& temporary, const char* name)
{
    const auto file = temporary.folder.getChildFile (
        juce::String (name).fromLastOccurrenceOf ("/", false, false).upToFirstOccurrenceOf (".", false, false) + ".phazite");
    REQUIRE (phazite::test::getGoldenFolder().getChildFile (name).copyFileTo (file));
    juce::String error;
    auto project = phazite::Project::open (file, false, error);
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    return project;
}
} // namespace

//==============================================================================
TEST_CASE ("Curve values: every curve of the golden files, read into values and written into an emptied AUTOMATION, is the "
           "curve it was, in its order")
{
    for (const auto& [name, automation] : readGoldenCurves())
    {
        INFO (juce::String (name));
        const auto value = phazite::readCurvesValue (automation);
        REQUIRE (holdsItsOrder (value));
        CHECK (toVector (value.order) == getParameters (automation));
        REQUIRE (! value.order.empty());

        const auto written = writeAll (value);
        const auto differences = findCurveDifferences (automation, written);
        CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n"));
        CHECK (getParameters (written) == getParameters (automation));
        CHECK (phazite::readCurvesValue (written) == value);

        // Each curve alone, as its element holds it, the same.
        for (const auto& curve : automation)
        {
            const auto parameter = phazite::readCurveParameter (curve);
            REQUIRE (value.find (parameter) != nullptr);
            CHECK (phazite::readCurveValue (curve) == *value.find (parameter));
            CHECK (phazite::holdsPoints (curve) == ! value.find (parameter)->points.empty());

            juce::ValueTree one (schema::AUTOMATION);
            phazite::writeCurveElement (parameter, *value.find (parameter), one);
            REQUIRE (one.getNumChildren() == 1);
            CHECK (findCurveDifferences (curve, one.getChild (0)).isEmpty());
        }
    }
}

TEST_CASE ("Curve values: the golden projects opened, their curves read through automation's readers are what their trees hold, "
           "read as before")
{
    phazite::test::TemporaryFolder temporary;

    for (const auto* name : { "projects/representative.xml", "commands/script.xml" })
    {
        INFO (juce::String (name));
        const auto project = openGolden (temporary, name);
        auto& song = project->getSong();
        const auto automation = phazite::test::copyWholeSong (song).getChildWithName (schema::AUTOMATION);
        REQUIRE (automation.getNumChildren() > 0);

        // The keys, in the tree's order; those with points; all the values at once.
        const auto keys = phazite::getCurveKeys (song);
        CHECK (keys == getParameters (automation));
        std::vector<ParameterRef> withPoints;

        for (const auto& curve : automation)
            if (curve.getNumChildren() > 0)
                withPoints.push_back (phazite::readCurveParameter (curve));

        CHECK (phazite::getCurveKeysWithPoints (song) == withPoints);
        const auto all = phazite::getCurvesValue (song);
        CHECK (holdsItsOrder (all));
        CHECK (all == phazite::readCurvesValue (automation));

        // Each curve: its value, its points, whether it has any, its positions, as played, and whole.
        std::vector<phazite::BeatPosition> beats;

        for (int i = -2; i < 80; ++i)
            beats.push_back (phazite::BeatPosition::fromBeats (i * 0.37));

        for (size_t i = 0; i < keys.size(); ++i)
        {
            const auto& key = keys[i];
            const auto element = automation.getChild ((int) i);
            CAPTURE (phazite::getCurveKeyText (key));

            const auto value = phazite::getCurveValue (song, key);
            REQUIRE (value.has_value());
            CHECK (*value == phazite::readCurveValue (element));
            CHECK (isSame (phazite::getPoints (song, key), readPointsAsBefore (element)));
            CHECK (phazite::hasPoints (song, key) == (element.getNumChildren() > 0));
            CHECK (isSame (toEnginePoints (*phazite::findCurve (song, key)), readPlayedAsBefore (element)));
            CHECK (phazite::copyWholeCurve (song, key).isEquivalentTo (element));

            const phazite::engine::Curve before (readPlayedAsBefore (element));
            const auto positions = phazite::getPositions (song, key, beats);
            REQUIRE (positions.size() == beats.size());

            for (size_t b = 0; b < beats.size(); ++b)
                CHECK (juce::exactlyEqual (positions[b], before.getPositionAt (std::max (0.0, beats[b].inBeats()))));
        }

        // A parameter without a curve: none, nothing played, nothing whole.
        const ParameterRef none { phazite::DeviceID::fromRaw (987654321), "volume" };
        CHECK_FALSE (phazite::getCurveValue (song, none).has_value());
        CHECK (phazite::getPoints (song, none).empty());
        CHECK_FALSE (phazite::hasPoints (song, none));
        CHECK (phazite::getPositions (song, none, beats).empty());
        CHECK (phazite::findCurve (song, none) == nullptr);
        CHECK_FALSE (phazite::copyWholeCurve (song, none).isValid());
    }
}

TEST_CASE ("Curve values: damaged curves read as automation's code read them, and written as it writes them, the differences "
           "listed")
{
    struct Damaged
    {
        const char* about;
        const char* xml;
    };

    // The first ones as InconsistentProjectTests.cpp damages the curves of the project it opens (a point's value of 12, a
    // curve of a device the song hasn't, a point without a value or with one that is no number, a curve whose points all
    // went, one holding only what Phazite doesn't know), which the loader makes consistent before anything reads them
    // (makeConsistent(): it takes out such a point, such a curve, and a curve's children that are no POINT, leaving that last
    // curve without points, as the reader reads it); the others as another tool's file or a hand's might hold one.
    const Damaged damaged[] = {
        { "a point's value of 12, and a curve of a device the song hasn't",
          R"(<AUTOMATION><CURVE device="14" parameter="volume"><POINT beat="0" value="12"/></CURVE><CURVE device="777777" parameter="volume"><POINT beat="0" value="0.5"/></CURVE></AUTOMATION>)" },
        { "a point without a value",
          R"(<AUTOMATION><CURVE device="14" parameter="volume"><POINT beat="0"/><POINT beat="8" value="0.8"/></CURVE></AUTOMATION>)" },
        { "a point whose value is no number",
          R"(<AUTOMATION><CURVE device="14" parameter="volume"><POINT beat="0" value="loud"/><POINT beat="8" value="0.8"/></CURVE></AUTOMATION>)" },
        { "a curve holding only what Phazite doesn't know",
          R"(<AUTOMATION><CURVE device="14" parameter="volume"><SHAPE kind="spline"/></CURVE></AUTOMATION>)" },
        { "a curve without points", R"(<AUTOMATION><CURVE device="14" parameter="volume"/></AUTOMATION>)" },
        { "two curves of one parameter, the first without points",
          R"(<AUTOMATION><CURVE device="14" parameter="cutoff"/><CURVE device="14" parameter="cutoff"><POINT beat="0" value="0.2"/></CURVE><CURVE device="15" parameter="cutoff"><POINT beat="2" value="0.4"/></CURVE></AUTOMATION>)" },
        { "a curve of 0 written, a curve beyond 1, a beat before the song",
          R"(<AUTOMATION><CURVE device="14" parameter="volume"><POINT beat="-4" value="0.1" curve="0"/><POINT beat="0" value="0.2" curve="3"/><POINT beat="4" value="0.3"/></CURVE></AUTOMATION>)" },
        { "points out of time order, two at one beat",
          R"(<AUTOMATION><CURVE device="14" parameter="volume"><POINT beat="8" value="0.1"/><POINT beat="0" value="0.2"/><POINT beat="8" value="0.3"/></CURVE></AUTOMATION>)" },
        { "a device's ID with leading zeros, and as a number",
          R"(<AUTOMATION><CURVE device="0014" parameter="volume"><POINT beat="0" value="0.5"/></CURVE><CURVE device="15" parameter="pan"><POINT beat="0" value="0.5"/></CURVE></AUTOMATION>)" },
        { "properties and children Phazite doesn't know",
          R"(<AUTOMATION colour="red"><CURVE device="14" parameter="volume" locked="1"><POINT beat="0" value="0.5" swing="0.2"/><NOTE pitch="60"/></CURVE><LINE/></AUTOMATION>)" },
        { "a beat that is absent",
          R"(<AUTOMATION><CURVE device="14" parameter="volume"><POINT value="0.5"/><POINT beat="2" value="0.6"/></CURVE></AUTOMATION>)" },
        { "empty", R"(<AUTOMATION/>)" },
    };

    for (const auto& each : damaged)
    {
        INFO (juce::String (each.about));
        auto automation = parse (each.xml);

        // The device's ID as a number rather than text, as a hand could write it ("a device's ID ... as a number").
        if (juce::String (each.about).contains ("as a number"))
            automation.getChild (1).setProperty (schema::curveDevice, 15, nullptr);

        const auto value = phazite::readCurvesValue (automation);
        const auto written = writeAll (value);
        CHECK (holdsItsOrder (value));

        // Written again from what it reads, the same text: what is written is the value whole.
        CHECK (writeAll (phazite::readCurvesValue (written)).toXmlString() == written.toXmlString());
        CHECK (phazite::readCurvesValue (written) == value);

        // Each curve (the first of a parameter: the one held) read as Phazite's engine played it, every POINT child, but one
        // without a value, which is no point.
        std::vector<ParameterRef> seen;

        for (const auto& curve : automation)
        {
            if (! curve.hasType (schema::CURVE))
                continue;

            const auto parameter = phazite::readCurveParameter (curve);

            if (std::find (seen.begin(), seen.end(), parameter) != seen.end())
                continue;

            seen.push_back (parameter);
            const auto* held = value.find (parameter);
            REQUIRE (held != nullptr);
            std::vector<phazite::engine::CurvePoint> expected;

            for (const auto& point : readPlayedAsBefore (curve))
                expected.push_back (point);

            if (juce::String (each.about) == "a point without a value")
                expected.erase (expected.begin()); // the one without, which read as 0 before

            CHECK (isSame (toEnginePoints (*held), expected));
        }

        // What differs from the curves as they were: listed, for the report.
        auto differences = findCurveDifferences (automation, written);

        if (getParameters (written) != getParameters (automation))
            differences.add ("curves: " + juce::String ((int) getParameters (automation).size()) + " -> "
                             + juce::String ((int) getParameters (written).size()));

        MESSAGE (juce::String (each.about) << ":\n  "
                                           << (differences.isEmpty() ? juce::String ("none") : differences.joinIntoString ("\n  ")));
    }

    // The two cases the conversion decides: a second curve of one parameter is no part of the value (the first is, as the
    // loader keeps it); a device's ID with leading zeros names the device its digits do, and is written without them.
    {
        const auto value = phazite::readCurvesValue (parse (damaged[5].xml));
        REQUIRE (value.order.size() == 2);
        CHECK (value.find ({ phazite::DeviceID::fromRaw (14), "cutoff" })->points.empty());
        CHECK (value.order[1] == ParameterRef { phazite::DeviceID::fromRaw (15), "cutoff" });
    }

    {
        const auto automation = parse (damaged[8].xml);
        const auto value = phazite::readCurvesValue (automation);
        CHECK (value.order[0] == ParameterRef { phazite::DeviceID::fromRaw (14), "volume" });
        CHECK (writeAll (value).getChild (0)[schema::curveDevice].toString() == "14");
    }

    // A curve holding only what Phazite doesn't know reads as one without points, and is written so: as the loader leaves it
    // (InconsistentProjectTests.cpp).
    {
        const auto value = phazite::readCurvesValue (parse (damaged[3].xml));
        REQUIRE (value.order.size() == 1);
        CHECK (value.find (value.order[0])->points.empty());
        const auto written = writeAll (value);
        REQUIRE (written.getNumChildren() == 1);
        CHECK (written.getChild (0).getNumChildren() == 0);
    }

    // A point whose value is no number reads as automation's code read it, at 0 (the loader takes it out as the song opens).
    {
        const auto value = phazite::readCurvesValue (parse (damaged[2].xml));
        REQUIRE (value.order.size() == 1);
        const auto* curve = value.find (value.order[0]);
        REQUIRE (curve->points.size() == 2);
        CHECK (juce::exactlyEqual (curve->points[0].position, 0.0f));
    }

    // Points out of time order stay in the element's order.
    {
        const auto value = phazite::readCurvesValue (parse (damaged[7].xml));
        const auto* curve = value.find (value.order[0]);
        REQUIRE (curve->points.size() == 3);
        CHECK (juce::exactlyEqual (curve->points[0].position, 0.1f));
        CHECK (juce::exactlyEqual (curve->points[1].position, 0.2f));
        CHECK (juce::exactlyEqual (curve->points[2].position, 0.3f));
    }
}

TEST_CASE ("Curve values: a curve read as the song holds one has each number within its range, without a point whose value is "
           "no number")
{
    // What enters the song's curves from another song reads them so (CurveValues.h: readHeldCurve()): a version's curves
    // applied, a merge's choice of a curve.
    using phazite::BeatPosition;
    using phazite::PointValue;

    // A beat before the song at 0, a value of 12 at 1, a bend of 3 at 1; a point without a value, and one whose value is no
    // number, left out; a beat that is absent at 0, a value below 0 at 0; a child that is no point left out; the element's
    // order kept.
    const auto held = phazite::readHeldCurve (parse (
        R"(<CURVE device="14" parameter="volume"><POINT beat="-4" value="12" curve="3"/><POINT beat="8"/><POINT value="0.5" curve="-0.25"/><POINT beat="2" value="loud"/><SHAPE/><POINT beat="1" value="-1"/></CURVE>)"));
    REQUIRE (held.points.size() == 3);
    CHECK (held.points[0] == PointValue { BeatPosition::fromBeats (0.0), 1.0f, 1.0f });
    CHECK (held.points[1] == PointValue { BeatPosition::fromBeats (0.0), 0.5f, -0.25f });
    CHECK (held.points[2] == PointValue { BeatPosition::fromBeats (1.0), 0.0f, 0.0f });

    // A curve within its ranges reads the same both ways.
    const auto within =
        parse (R"(<CURVE device="14" parameter="volume"><POINT beat="0" value="0.2"/><POINT beat="4.5" value="0.8" curve="0.5"/></CURVE>)");
    CHECK (phazite::readHeldCurve (within) == phazite::readCurveValue (within));
    CHECK (phazite::readHeldCurve (within).points.size() == 2);
}

TEST_CASE ("Curve values: the song's curves keep the order they were made in through their helpers, and compare by identity and "
           "by what they hold")
{
    const ParameterRef a { phazite::DeviceID::fromRaw (3), "cutoff" }, b { phazite::DeviceID::fromRaw (1), "volume" },
        c { phazite::DeviceID::fromRaw (2), "pan" }, d { phazite::DeviceID::fromRaw (9), "volume" };
    const auto curveOf = [] (std::initializer_list<float> positions)
    {
        CurveValue curve;
        auto beat = 0.0;

        for (const auto position : positions)
            curve.points = curve.points.push_back ({ phazite::BeatPosition::fromBeats (beat++), position, 0.0f });

        return curve;
    };

    SongCurves curves;
    curves = withCurve (curves, a, curveOf ({ 0.1f }));
    curves = withCurve (curves, b, curveOf ({ 0.2f, 0.3f }));
    curves = withCurve (curves, c, curveOf ({ 0.4f }));
    CHECK (toVector (curves.order) == std::vector { a, b, c });
    CHECK (holdsItsOrder (curves));

    // Changed in its place; taken out; renamed in its place.
    auto changed = withCurve (curves, a, curveOf ({ 0.5f }));
    CHECK (toVector (changed.order) == std::vector { a, b, c });
    CHECK (juce::exactlyEqual (changed.find (a)->points[0].position, 0.5f));
    CHECK (toVector (withoutCurve (curves, b).order) == std::vector { a, c });
    CHECK (withoutCurve (curves, b).find (b) == nullptr);
    CHECK (withoutCurve (curves, d).isSameAs (curves));
    const auto renamed = withCurveRenamed (curves, b, d);
    CHECK (toVector (renamed.order) == std::vector { a, d, c });
    CHECK (renamed.find (b) == nullptr);
    CHECK (*renamed.find (d) == *curves.find (b));
    CHECK (holdsItsOrder (renamed));

    // Written in their order, and read back the same.
    CHECK (getParameters (writeAll (curves)) == std::vector { a, b, c });
    CHECK (phazite::readCurvesValue (writeAll (curves)) == curves);

    // The same by identity only while shared; the same by what they hold.
    auto copy = curves;
    CHECK (copy.isSameAs (curves));
    copy = withCurve (copy, a, *curves.find (a));
    CHECK_FALSE (copy.isSameAs (curves));
    CHECK (copy == curves);
    CHECK_FALSE (withCurve (curves, a, curveOf ({ 0.1f, 0.1f })) == curves);
    CHECK_FALSE (withCurve (withoutCurve (curves, a), a, *curves.find (a)) == curves); // the same curves in another order

    auto curve = *curves.find (b);
    CHECK (curve.isSameAs (*curves.find (b)));
    curve.points = curve.points.set (0, curve.points[0]);
    CHECK_FALSE (curve.isSameAs (*curves.find (b)));
    CHECK (curve == *curves.find (b));

    // Their points' keys aside, which no file holds: a point, a curve and the song's curves keyed otherwise the same, and so
    // to the comparisons of what songs keep (WholeState.h); a point's content still compared.
    auto keyed = curve.points[0];
    keyed.key = 77;
    CHECK (keyed == curve.points[0]);
    auto otherKeys = *curves.find (b);
    otherKeys.points = otherKeys.points.set (0, keyed);
    CHECK (otherKeys == *curves.find (b));
    const auto keyedCurves = withCurve (curves, b, otherKeys);
    CHECK_FALSE (keyedCurves.isSameAs (curves));
    CHECK (keyedCurves == curves);
    // What a song keeps, with these curves alone (by the member's name: the next area adds a member).
    const auto keepCurves = [] (const phazite::SongCurves& held)
    {
        phazite::SongState valuesKept;
        valuesKept.curves = held;
        return valuesKept;
    };
    const auto kept = keepCurves (curves), keptKeyed = keepCurves (keyedCurves);
    CHECK (phazite::holdSameCurve (kept, keptKeyed, b));
    CHECK (phazite::holdSameCurves (kept, keptKeyed));
    CHECK (phazite::holdSameCurveOrder (kept, keptKeyed));
    keyed.position = 0.25f;
    CHECK_FALSE (keyed == curve.points[0]);
    const auto keptMoved = keepCurves (withCurve (curves, b, { otherKeys.points.set (0, keyed) }));
    CHECK_FALSE (phazite::holdSameCurve (kept, keptMoved, b));
    CHECK_FALSE (phazite::holdSameCurves (kept, keptMoved));

    // The key text the versions name a curve by, and the parameter it names.
    CHECK (phazite::getCurveKeyText (a) == "3/cutoff");
    CHECK (phazite::readCurveKeyText ("3/cutoff") == a);
    CHECK (phazite::readCurveKeyText ("0003/cut/off") == ParameterRef { phazite::DeviceID::fromRaw (3), "cut/off" });
    CHECK_FALSE (phazite::readCurveKeyText ("cutoff").has_value());

    juce::ValueTree element (schema::CURVE);
    element.setProperty (schema::curveDevice, "3", nullptr);
    element.setProperty (schema::curveParameter, "cutoff", nullptr);
    CHECK (phazite::readCurveKeyText (schema::getElementKey (element)) == a);
}

TEST_CASE ("Curve values: a song opened holds its curves in its values, each point with a key of its own that no file holds, its "
           "AUTOMATION an empty place")
{
    // Roadmap step S5.20, automation's switch (CurvesArea.h): the song takes its file's curves in as it opens, each point given
    // a key from the song's count (SongValues.h: givePointKey()), which a whole copy never writes.
    phazite::test::TemporaryFolder temporary;
    const auto project = openGolden (temporary, "projects/representative.xml");
    auto& song = project->getSong();
    const auto& curves = phazite::getCurvesValue (song);
    REQUIRE (! curves.order.empty());
    std::vector<std::uint64_t> keys;

    for (const auto& parameter : curves.order)
    {
        REQUIRE (curves.find (parameter) != nullptr);
        CHECK (phazite::findCurve (song, parameter) == curves.find (parameter)); // in place

        for (const auto& point : curves.find (parameter)->points)
            keys.push_back (point.key);
    }

    std::sort (keys.begin(), keys.end());
    REQUIRE (! keys.empty());
    CHECK (keys.front() > 0);
    CHECK (keys.back() < song.getValues()->getNextPointKey());
    CHECK (std::adjacent_find (keys.begin(), keys.end()) == keys.end());

    // Written as before: beat, value, and a bend that isn't 0, no key.
    for (const auto& curve : phazite::test::copyWholeSong (song).getChildWithName (schema::AUTOMATION))
        for (const auto& point : curve)
            for (int i = 0; i < point.getNumProperties(); ++i)
                CHECK (juce::StringArray { "beat", "value", "curve" }.contains (point.getPropertyName (i).toString()));
}
