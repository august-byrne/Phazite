// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's sends as values (model/base/internal/SendsValue.h): the conversion
// between a SEND element and its value (which the sends' readers read a send with), round trip through the golden
// project's sends and through damaged ones, the helpers that alone write the song's sends, the IDs they hold and name; the
// sends' readers by ID (model/mixer/internal/Mixer.h), the same as the tree; and the readers that would find nothing without
// a word if they missed the sends (a send's lane among its track's, a send's selected points, the DAWproject export), each
// tested through the model, as the song is built, so that it holds across the sends' leaving the tree.
//   phazite_tests.exe --test-case="Sends value*"
#include "GoldenFiles.h"
#include "SongCheck.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/eq/Equalizer.h"
#include "devices/reverb/Reverb.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/SongText.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::DeviceID;
using phazite::SendValue;
using phazite::SongSends;
using phazite::TrackID;
using phazite::test::checkSong;
using phazite::test::TestSong;

juce::ValueTree parse (const juce::String& xml)
{
    const auto parsed = juce::parseXML (xml);
    REQUIRE (parsed != nullptr);
    return juce::ValueTree::fromXml (*parsed);
}

phazite::BeatPosition at (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

// Each TRACK under an element, a folder before the tracks in it.
void forEachTrack (const juce::ValueTree& parent, const std::function<void (juce::ValueTree)>& visit)
{
    for (const auto& child : parent)
    {
        if (child.hasType (schema::TRACKS) || child.hasType (schema::TRACK))
        {
            if (child.hasType (schema::TRACK))
                visit (child);

            forEachTrack (child, visit);
        }
    }
}

// Whether two SEND elements hold the same send's properties, each written as the same text (SongText.h: isSameValue()).
void checkSameSend (const juce::ValueTree& expected, const juce::ValueTree& actual)
{
    for (const auto* property :
         { &schema::sendID.getName(), &schema::sendTo.getName(), &schema::sendLevel.getName(), &schema::sendPreFader.getName() })
    {
        INFO (property->toString());
        REQUIRE (actual.hasProperty (*property) == expected.hasProperty (*property));

        if (expected.hasProperty (*property))
            CHECK (phazite::isSameValue (actual[*property], expected[*property]));
    }
}

// Every SEND of a song's whole copy put back as its value writes it, in its place: what a save writes once the sends are
// values.
juce::ValueTree rewriteSends (juce::ValueTree whole)
{
    forEachTrack (whole,
                  [] (juce::ValueTree track)
                  {
                      const auto from = TrackID::fromRaw (phazite::readID (track[schema::trackID]));

                      for (int i = 0; i < track.getNumChildren(); ++i)
                      {
                          if (const auto send = phazite::readHeldSend (track.getChild (i), from))
                          {
                              track.removeChild (i, nullptr);
                              track.addChild (phazite::writeSendElement (*send), i, nullptr);
                          }
                      }
                  });

    return whole;
}

juce::String writeText (const juce::ValueTree& song)
{
    juce::MemoryOutputStream out;
    phazite::writeProjectText (out, song);
    return out.toString();
}

SendValue makeSend (std::uint64_t id, std::uint64_t from, std::uint64_t to, float level = 0.0f, bool preFader = false)
{
    return { DeviceID::fromRaw (id), TrackID::fromRaw (from), TrackID::fromRaw (to), level, preFader };
}

std::vector<DeviceID> listOf (const SongSends& sends, std::uint64_t track)
{
    const auto* list = sends.tracks.find (track);
    return list != nullptr ? std::vector<DeviceID> (list->begin(), list->end()) : std::vector<DeviceID>();
}

std::vector<DeviceID> ids (std::initializer_list<std::uint64_t> raw)
{
    std::vector<DeviceID> made;

    for (const auto each : raw)
        made.push_back (DeviceID::fromRaw (each));

    return made;
}

// A track, with an equalizer (which a DAWproject export bounces: no other DAW plays Phazite's devices) or without, sending to
// a reverb's return, as the model builds them, and the send.
struct Sending
{
    TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    TrackID track = s.addTrack ("Lead");
    DeviceID equalizer;
    TrackID reverb = s.addReturn (track, phazite::devices::Reverb::typeID);
    DeviceID send = phazite::getSends (song, track).front();
    phazite::ParameterRef sendLevel { send, schema::sendLevelParameter };

    explicit Sending (bool withEqualizer = true)
    {
        if (withEqualizer)
            equalizer = s.addEffect (track, phazite::devices::Equalizer::typeID);
    }

    // A parameter of the equalizer's that a lane can show.
    phazite::ParameterRef findEqualizerParameter() const
    {
        for (const auto& parameter : phazite::getAutomatableParameters (song, track))
            if (parameter.device == equalizer)
                return parameter;

        FAIL ("the equalizer has no parameter a lane shows");
        return {};
    }
};
} // namespace

TEST_CASE ("Sends value: the golden project's sends, read and written, are the sends they were, and the song written with them is "
           "the same text")
{
    const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (xml != nullptr);
    const auto file = juce::ValueTree::fromXml (*xml);
    auto count = 0;

    forEachTrack (file,
                  [&count] (juce::ValueTree track)
                  {
                      const auto from = TrackID::fromRaw (phazite::readID (track[schema::trackID]));

                      for (const auto& child : track)
                      {
                          if (! child.hasType (schema::SEND))
                              continue;

                          ++count;
                          const auto held = phazite::readHeldSend (child, from);
                          REQUIRE (held.has_value());
                          CHECK (*held == phazite::readSendValue (child, from));
                          CHECK (held->from == from);
                          CHECK (held->id.getRaw() == phazite::readID (child[schema::sendID]));
                          checkSameSend (child, phazite::writeSendElement (*held));
                          CHECK (phazite::readHeldSend (phazite::writeSendElement (*held), from) == held);
                      }
                  });

    CHECK (count == 2);
    const auto all = phazite::readSendsValue (file);
    CHECK (all.sends.size() == 2);
    CHECK (phazite::holdsItsLists (all));

    // Opened as a song, written whole with every send put back as its value writes it: the same text, twice.
    const auto song = phazite::makeSong (file);
    REQUIRE (song != nullptr);
    CHECK (phazite::getSendsValue (*song) == all);
    const auto whole = phazite::test::copyWholeSong (*song);
    const auto written = writeText (whole);
    const auto rewritten = rewriteSends (whole.createCopy());
    CHECK (writeText (rewritten) == written);
    CHECK (writeText (rewriteSends (rewritten)) == written);
}

TEST_CASE ("Sends value: damaged sends read as the sends' code read them, written as it writes them, and the same written twice")
{
    // A send with numbers as text, one beyond its level's range, one with a level that is no number, one before its fader
    // written "0", one without an ID, one with an ID past those the song counts on, a child of another type and a property
    // Phazite doesn't know.
    const auto track = parse (R"(<TRACK id="5" kind="track">)"
                              R"(<SEND id=" 21 " to="32" level="-12.5" preFader="1"/>)"
                              R"(<SEND id="22" to="32" level="30"/>)"
                              R"(<SEND id="23" to="32" level="loud" preFader="0" shape="round"/>)"
                              R"(<OTHER id="24" to="32"/>)"
                              R"(<SEND to="32" level="-3"/>)"
                              R"(<SEND id="4611686018427387904" to="32"/>)"
                              R"(<SEND id="25" to="33" level="-3.14159"/>)"
                              R"(</TRACK>)");

    const auto sends = phazite::readTrackSends (track);
    REQUIRE (sends.size() == 6);
    CHECK (sends[0] == makeSend (21, 5, 32, -12.5f, true));
    CHECK (sends[1] == makeSend (22, 5, 32, 6.0f)); // within its range
    CHECK (sends[2] == makeSend (23, 5, 32, 0.0f)); // a level that is no number: none; "0": after the fader
    CHECK (sends[3] == makeSend (0, 5, 32, -3.0f)); // without an ID: read, though the song holds none so
    CHECK (sends[4].id.getRaw() == phazite::schema::maxID);
    CHECK (sends[5] == makeSend (25, 5, 33, -3.14159f));

    // As the song holds them: none without an ID it can count on.
    CHECK (phazite::readHeldSend (track.getChild (0), TrackID::fromRaw (5)) == sends[0]);
    CHECK_FALSE (phazite::readHeldSend (track.getChild (3), TrackID::fromRaw (5)).has_value()); // not a send
    CHECK_FALSE (phazite::readHeldSend (track.getChild (4), TrackID::fromRaw (5)).has_value());
    CHECK_FALSE (phazite::readHeldSend (track.getChild (5), TrackID::fromRaw (5)).has_value());
    CHECK (phazite::findTrackSend (track, DeviceID::fromRaw (22)) == sends[1]);
    CHECK_FALSE (phazite::findTrackSend (track, DeviceID::fromRaw (24)).has_value());
    CHECK_FALSE (phazite::findTrackSend (track, DeviceID()).has_value());

    // Written as the sends' code writes them: the ID and the return as numbers, the level only while it isn't 0 dB, before
    // the fader only while it is, and nothing Phazite doesn't know; read back the same, and written the same again.
    for (const auto& send : sends)
    {
        const auto written = phazite::writeSendElement (send);
        CHECK (written.hasType (schema::SEND));
        CHECK (written.getNumProperties() == 2 + (juce::exactlyEqual (send.level, 0.0f) ? 0 : 1) + (send.preFader ? 1 : 0));
        CHECK (written[schema::sendID].isInt64());
        CHECK (written[schema::sendTo].isInt64());
        CHECK (phazite::readSendValue (written, send.from) == send);
        checkSameSend (written, phazite::writeSendElement (phazite::readSendValue (written, send.from)));
    }

    CHECK (phazite::writeValue (phazite::writeSendElement (sends[5])[schema::sendLevel]) == "-3.14159"); // as the file held it
    CHECK (phazite::writeValue (phazite::writeSendElement (sends[0])[schema::sendLevel]) == "-12.5");
    CHECK_FALSE (phazite::writeSendElement (sends[2]).hasProperty (schema::sendPreFader));
    CHECK (phazite::readTrackSends (parse ("<MASTER id=\"1\"><SEND id=\"7\" to=\"32\"/></MASTER>")).empty());

    // Opened in a song (a file from others, which the loader makes consistent): each send the loader leaves reads as the
    // song holds it, and its value written and read again is the same value, the song's text the same written twice.
    const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (xml != nullptr);
    auto file = juce::ValueTree::fromXml (*xml);
    juce::ValueTree sender;
    forEachTrack (file,
                  [&sender] (juce::ValueTree each)
                  {
                      if (! sender.isValid() && each.getChildWithName (schema::SEND).isValid())
                          sender = each;
                  });

    REQUIRE (sender.isValid());
    const auto returnID = sender.getChildWithName (schema::SEND)[schema::sendTo].toString();
    const auto returnRaw = phazite::readID (sender.getChildWithName (schema::SEND)[schema::sendTo]);
    REQUIRE (phazite::readTrackSends (sender).size() == 1); // the golden project's one send on the track

    for (const auto* damaged :
         { R"(<SEND id="900001" to="RETURN" level="-12.34567" preFader="1"/>)",
           R"(<SEND id="900002" to="RETURN" level="30" preFader="0"/>)", R"(<SEND id="900003" to="RETURN" level="nan"/>)",
           R"(<SEND to="RETURN" level="-3"/>)", R"(<SEND id="900001" to="RETURN" level="-1"/>)", R"(<SEND id="900005" to="900999"/>)" })
        sender.appendChild (parse (juce::String (damaged).replace ("RETURN", returnID)), nullptr);

    const auto song = phazite::makeSong (file);
    REQUIRE (song != nullptr);
    checkSong (*song);
    const auto senderID = TrackID::fromRaw (phazite::readID (sender[schema::trackID]));
    const auto opened = phazite::getSendValues (*song, senderID);

    // The golden project's send, then each damaged one in its order but the one to a return the song hasn't (900005), which
    // goes as the song opens: the levels kept within their range and to a ten-thousandth, as the song holds a send (from the
    // sends' switch: SendsValue.h, readHeldSend(), the loader's keepSendAsHeld()), and the one without an ID and the second
    // with 900001 each given an ID of its own.
    REQUIRE (opened.size() == 6);
    const auto senderRaw = senderID.getRaw();
    CHECK (opened[1] == makeSend (900001, senderRaw, returnRaw, -12.3457f, true));
    CHECK (opened[2] == makeSend (900002, senderRaw, returnRaw, 6.0f));
    CHECK (opened[3] == makeSend (900003, senderRaw, returnRaw, 0.0f));

    for (const auto& [index, level] : { std::pair (4, -3.0f), std::pair (5, -1.0f) })
    {
        const auto& given = opened[(size_t) index];
        INFO ("the send at " << index);
        CHECK (given == makeSend (given.id.getRaw(), senderRaw, returnRaw, level));
        CHECK (given.id.getRaw() < phazite::schema::maxID);
        CHECK (given.id.getRaw() != 900001);
    }

    std::set<DeviceID> seen;

    for (const auto& send : opened)
    {
        CHECK (send.id.isValid());
        CHECK (seen.insert (send.id).second); // each ID once
        CHECK (send.level >= -100.0f);
        CHECK (send.level <= 6.0f);
        CHECK (phazite::readHeldSend (phazite::writeSendElement (send), senderID) == send);
        CHECK (phazite::getSendValue (*song, send.id) == send);
    }

    const auto whole = phazite::test::copyWholeSong (*song);
    const auto rewritten = rewriteSends (whole.createCopy());
    CHECK (writeText (rewriteSends (rewritten)) == writeText (rewritten));
}

TEST_CASE ("Sends value: a send's property set as its element would hold it, and the song's sends written by their helpers alone")
{
    // A merge's clash chosen: each property as the element would read it, a void value as none.
    auto send = makeSend (21, 5, 32, -6.0f, true);
    CHECK (phazite::setSendProperty (send, schema::sendLevel, "-12"));
    CHECK (juce::exactlyEqual (send.level, -12.0f));
    CHECK (phazite::setSendProperty (send, schema::sendLevel, 99.0));
    CHECK (juce::exactlyEqual (send.level, 6.0f));
    CHECK (phazite::setSendProperty (send, schema::sendLevel, {}));
    CHECK (juce::exactlyEqual (send.level, 0.0f));
    CHECK (phazite::setSendProperty (send, schema::sendPreFader, {}));
    CHECK_FALSE (send.preFader);
    CHECK (phazite::setSendProperty (send, schema::sendTo, "40"));
    CHECK (send.to == TrackID::fromRaw (40));
    CHECK (phazite::setSendProperty (send, schema::sendID, (juce::int64) 77));
    CHECK (send.id == DeviceID::fromRaw (77));
    CHECK_FALSE (phazite::setSendProperty (send, juce::Identifier ("shape"), "round"));
    CHECK (send == makeSend (77, 5, 40));

    // Added at the end of their tracks' lists or at a place, changed in place, moved to another track, taken out.
    SongSends sends;
    sends = phazite::withSend (sends, makeSend (1, 10, 90));
    sends = phazite::withSend (sends, makeSend (2, 10, 90));
    sends = phazite::withSend (sends, makeSend (3, 10, 91), 0);
    sends = phazite::withSend (sends, makeSend (4, 11, 90));
    CHECK (listOf (sends, 10) == ids ({ 3, 1, 2 }));
    CHECK (listOf (sends, 11) == ids ({ 4 }));
    CHECK (phazite::holdsItsLists (sends));

    const auto before = sends;
    sends = phazite::withSend (sends, makeSend (1, 10, 90, -3.0f));
    CHECK (listOf (sends, 10) == ids ({ 3, 1, 2 })); // in its place
    CHECK (juce::exactlyEqual (sends.find (DeviceID::fromRaw (1))->level, -3.0f));
    CHECK_FALSE (sends == before);

    sends = phazite::withSend (sends, makeSend (2, 11, 90));
    CHECK (listOf (sends, 10) == ids ({ 3, 1 }));
    CHECK (listOf (sends, 11) == ids ({ 4, 2 }));

    sends = phazite::withTrackSendOrder (sends, TrackID::fromRaw (10),
                                         immer::flex_vector<DeviceID> { DeviceID::fromRaw (1), DeviceID::fromRaw (3) });
    CHECK (listOf (sends, 10) == ids ({ 1, 3 }));
    const auto ordered = sends;
    CHECK (phazite::withTrackSendOrder (sends, TrackID::fromRaw (10),
                                        immer::flex_vector<DeviceID> { DeviceID::fromRaw (1), DeviceID::fromRaw (3) })
               .isSameAs (ordered));

    sends = phazite::withoutSend (sends, DeviceID::fromRaw (4));
    sends = phazite::withoutSend (sends, DeviceID::fromRaw (2));
    CHECK (sends.tracks.find (11) == nullptr); // its last send gone, its list too
    CHECK (phazite::withoutSend (sends, DeviceID::fromRaw (99)).isSameAs (sends));
    CHECK (phazite::holdsItsLists (sends));

    // What isn't held as the helpers hold it.
    auto broken = sends;
    broken.tracks = broken.tracks.set (12, immer::flex_vector<DeviceID> { DeviceID::fromRaw (1) });
    CHECK_FALSE (phazite::holdsItsLists (broken));
    broken = sends;
    broken.sends = broken.sends.set (8, makeSend (8, 12, 90));
    CHECK_FALSE (phazite::holdsItsLists (broken));
    CHECK (phazite::holdsItsLists (phazite::withSend (sends, makeSend (9, 0, 90)))); // a send on none, in no list

    // The IDs they hold (each send's own) and name (each send's return), and the largest held below those the song counts on.
    sends = phazite::withSend (sends, makeSend (phazite::schema::maxID + 5, 10, 92));
    std::set<std::uint64_t> held, named;
    phazite::forEachIDHeld (sends, [&held] (std::uint64_t id) { held.insert (id); });
    phazite::forEachIDNamed (sends, [&named] (std::uint64_t id) { named.insert (id); });
    CHECK (held == std::set<std::uint64_t> { 1, 3, phazite::schema::maxID + 5 });
    CHECK (named == std::set<std::uint64_t> { 90, 91, 92 });
    CHECK (phazite::findLargestIDIn (sends) == 3);
    CHECK (phazite::findLargestIDIn (SongSends {}) == 0);
}

TEST_CASE ("Sends value: the song's sends read whole, one of each ID, and the sends' readers by ID the same as the tree's")
{
    // From a song's tree: each track's sends in its order, a folder's before those of the tracks in it, the first of an ID.
    const auto song = parse (R"(<PHAZITE><TRACKS>)"
                             R"(<TRACK id="10" kind="folder"><SEND id="1" to="30"/>)"
                             R"(<TRACK id="11" kind="track"><SEND id="2" to="30"/><SEND id="1" to="31"/></TRACK></TRACK>)"
                             R"(<TRACK id="12" kind="track"><SEND id="3" to="30" level="-2"/><SEND to="30"/></TRACK>)"
                             R"(</TRACKS></PHAZITE>)");
    const auto sends = phazite::readSendsValue (song);
    CHECK (sends.sends.size() == 3);
    CHECK (listOf (sends, 10) == ids ({ 1 }));
    CHECK (listOf (sends, 11) == ids ({ 2 }));
    CHECK (listOf (sends, 12) == ids ({ 3 }));
    CHECK (sends.find (DeviceID::fromRaw (1))->to == TrackID::fromRaw (30));
    CHECK (phazite::holdsItsLists (sends));
    CHECK (phazite::readSendsValue ({}) == SongSends {});

    // The readers by ID on a song built through the model.
    Sending sending;
    auto& model = sending.song;
    const auto value = phazite::getSendValue (model, sending.send);
    REQUIRE (value.has_value());
    CHECK (value->from == sending.track);
    CHECK (value->to == sending.reverb);
    CHECK (phazite::getSendTrack (model, sending.send) == sending.track);
    CHECK (phazite::getSendTarget (model, sending.send) == sending.reverb);
    CHECK (phazite::getSendValues (model, sending.track) == std::vector<SendValue> { *value });
    CHECK (phazite::getSendValues (model, sending.reverb).empty());
    CHECK (phazite::getSendValues (model, phazite::getMasterID (model)).empty());
    CHECK (phazite::holdsSend (model, sending.send));
    CHECK_FALSE (phazite::holdsSend (model, sending.equalizer));
    CHECK_FALSE (phazite::getSendValue (model, sending.equalizer).has_value());
    CHECK_FALSE (phazite::getSendTrack (model, DeviceID()).isValid());

    phazite::setSendLevel (model, sending.send, -9.5f);
    phazite::setPreFader (model, sending.send, true);
    CHECK (juce::exactlyEqual (phazite::getSendValue (model, sending.send)->level, -9.5f));
    CHECK (juce::exactlyEqual (phazite::getSendLevel (model, sending.send), -9.5f));
    CHECK (phazite::isPreFader (model, sending.send));
    CHECK (phazite::getSendsValue (model).sends.size() == 1);
    CHECK (phazite::getSendsValue (model) == phazite::readSendsValue (phazite::test::copyWholeSong (model)));

    // A control of its level finds it, reads it and follows it, and lets it go once it is gone.
    auto handle = phazite::getSendLevel (sending.s.context, sending.send);
    REQUIRE (handle.isValid());
    CHECK (handle.getValue() == doctest::Approx (phazite::decibelsToFaderPosition (-9.5f)));
    phazite::setSendLevel (model, sending.send, -3.0f);
    CHECK (handle.getValue() == doctest::Approx (phazite::decibelsToFaderPosition (-3.0f)));
    phazite::removeSend (model, sending.send);
    CHECK_FALSE (phazite::getSendValue (model, sending.send).has_value());
    CHECK_FALSE (phazite::holdsSend (model, sending.send));
    CHECK_FALSE (phazite::locateParameter (model, sending.sendLevel).has_value());
}

TEST_CASE ("Sends value: a send's automation lane keeps its place among its track's lanes, after the fader's and before its "
           "devices'")
{
    // A track's lanes, in the order they show.
    const auto findLanes = [] (const phazite::Song& song, TrackID track)
    {
        std::vector<phazite::ParameterRef> shown;

        for (const auto& lane : phazite::getLanes (song, track))
            shown.push_back (lane.parameter);

        return shown;
    };

    // Lanes the automation alone shows (none kept), each by its device's place: the send's before the equalizer's, though the
    // equalizer's points came first.
    {
        Sending sending;
        auto& song = sending.song;
        const auto equalizer = sending.findEqualizerParameter();
        REQUIRE (phazite::addPoint (song, equalizer, { at (1.0), 0.25f }) >= 0);
        REQUIRE (phazite::addPoint (song, sending.sendLevel, { at (2.0), 0.5f }) >= 0);
        CHECK (findLanes (song, sending.track) == std::vector<phazite::ParameterRef> { sending.sendLevel, equalizer });
        CHECK (phazite::getParameterTrack (song, sending.sendLevel) == sending.track);
        CHECK (phazite::getLanes (song, sending.reverb).empty()); // the return's lanes hold none of it
    }

    // Lanes shown on purpose, without points: each after those shown before it, the send's too.
    {
        Sending sending;
        auto& song = sending.song;
        const auto equalizer = sending.findEqualizerParameter();
        phazite::showLane (song, sending.track, equalizer);
        phazite::showLane (song, sending.track, sending.sendLevel);
        CHECK (findLanes (song, sending.track) == std::vector<phazite::ParameterRef> { equalizer, sending.sendLevel });
    }
}

TEST_CASE ("Sends value: a send's selected automation points count as selected while the send is there")
{
    Sending sending;
    auto& song = sending.song;
    auto& selection = phazite::getSelection (sending.s.context);

    for (int i = 0; i < 3; ++i)
        REQUIRE (phazite::addPoint (song, sending.sendLevel, { at (i * 2.0), 0.2f + (float) i * 0.2f }) >= 0);

    phazite::selectPoints (selection, sending.sendLevel, { 0, 2 });
    CHECK (selection.getNumObjectsSelected() == 2);
    const auto selected = phazite::getSelectedPoints (selection);
    REQUIRE (selected.has_value());
    CHECK (selected->indices == std::vector<int> { 0, 2 });

    // Its send gone, none of them counts (the curve stays, for the removal's Undo); back, they count again.
    phazite::removeSend (song, sending.send);
    CHECK (selection.getNumObjectsSelected() == 0);
    phazite::undo (song);
    CHECK (selection.getNumObjectsSelected() == 2);
}

TEST_CASE ("Sends value: a DAWproject export writes a send, after the fader or before it, with its level and its level's "
           "automation")
{
    Sending sending { false }; // no device of Phazite's own: the track isn't bounced
    auto& song = sending.song;
    phazite::setSendLevel (song, sending.send, -12.0f);
    phazite::setPreFader (song, sending.send, true);
    REQUIRE (phazite::addPoint (song, sending.sendLevel, { at (0.0), 0.0f }) >= 0);
    REQUIRE (phazite::addPoint (song, sending.sendLevel, { at (4.0), phazite::decibelsToFaderPosition (0.0f) }) >= 0);

    const auto content = phazite::describeDawProject (song, "Sends", {});
    REQUIRE (content.project != nullptr);
    const auto channelID = "c" + juce::String ((juce::int64) sending.track.getRaw());
    const auto sendID = channelID + "s" + juce::String ((juce::int64) sending.send.getRaw());

    const juce::XmlElement* send = nullptr;
    const juce::XmlElement* points = nullptr;
    const std::function<void (const juce::XmlElement&)> find = [&] (const juce::XmlElement& in)
    {
        for (const auto* child : in.getChildIterator())
        {
            if (child->hasTagName ("Send") && child->getStringAttribute ("id") == sendID)
                send = child;

            if (child->hasTagName ("Points") && child->getStringAttribute ("id") == "q" + sendID + "v")
                points = child;

            find (*child);
        }
    };

    find (*content.project);
    REQUIRE (send != nullptr);
    CHECK (send->getStringAttribute ("destination") == "c" + juce::String ((juce::int64) sending.reverb.getRaw()));
    CHECK (send->getStringAttribute ("type") == "pre");
    REQUIRE (send->getChildByName ("Volume") != nullptr);
    CHECK (send->getChildByName ("Volume")->getDoubleAttribute ("value")
           == doctest::Approx (juce::Decibels::decibelsToGain (-12.0)).epsilon (0.001));
    REQUIRE (points != nullptr);
    CHECK (points->getStringAttribute ("unit") == "linear");
    CHECK (points->getNumChildElements() >= 3); // its target and its points
}
