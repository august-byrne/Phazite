// SPDX-License-Identifier: AGPL-3.0-or-later
// Merging two lines of a song by IDs (docs/versions.md: Merging by IDs, step V4): each side a song of its own made from the
// same base, changed through the model, and the other side's lines taken into this side's.
#include "EnginePlayback.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/drums/DrumSampler.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/VersionHistory.h"

#include <set>

namespace
{
using namespace phazite::test;
namespace schema = phazite::schema;

// A base song, and two sides made from it, each a song of its own counting its IDs on from the same place.
struct Sides
{
    phazite::Song::Ptr base = phazite::createSong();
    std::vector<phazite::TrackID> tracks;
    phazite::ClipID clip;
    phazite::Song::Ptr ours, theirs;

    Sides()
    {
        tracks = phazite::ensureTracks (*base, 2);
        phazite::setTrackName (*base, tracks[0], "Bass");
        phazite::setTrackName (*base, tracks[1], "Drums");
        clip = addMidiClip (*base, tracks[0], { at (0.0), at (8.0) }, "Riff");
        addMidiNote (*base, clip, 36, at (0.0), lasting (1.0));
        ours = phazite::makeSong (base->getState().createCopy());
        theirs = phazite::makeSong (base->getState().createCopy());
    }

    // The other side's changes, every line of them, merged into this side's.
    phazite::MergeResult merge()
    {
        phazite::MergeLines lines;

        for (const auto& change : phazite::findChanges (base->getState(), theirs->getState()))
        {
            if (change.where == phazite::SongChange::Where::song)
                lines.songOwn = true;
            else
                lines.tracks.insert (change.trackID);
        }

        return phazite::mergeSongs (base->getState(), ours->getState(), theirs->getState(), lines, [this] { return ours->makeID(); });
    }
};

juce::ValueTree findTrack (const juce::ValueTree& song, const juce::String& name)
{
    for (const auto& track : phazite::getTrackElements (song))
        if (track[schema::trackName].toString() == name)
            return track;

    return {};
}

// Every item's ID once in the song, notes' once in their clips, and what names an item names one the song has.
void checkIDs (const juce::ValueTree& song)
{
    std::set<std::uint64_t> items;
    const std::function<void (const juce::ValueTree&)> collect = [&] (const juce::ValueTree& tree)
    {
        std::set<std::uint64_t> notes;

        for (const auto& child : tree)
        {
            if (child.hasType (schema::NOTE))
            {
                INFO ("a note's ID twice in its clip");
                CHECK (notes.insert (phazite::readID (child[schema::noteID])).second);
                continue;
            }

            for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA })
            {
                if (child.hasProperty (*property) && schema::holdsID (child, *property))
                {
                    INFO ((child.getType().toString() + " " + child[*property].toString()).toStdString());
                    CHECK (items.insert (phazite::readID (child[*property])).second);
                }
            }

            collect (child);
        }
    };

    collect (song);

    const std::function<void (const juce::ValueTree&)> check = [&] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            for (int i = 0; i < child.getNumProperties(); ++i)
            {
                if (const auto property = child.getPropertyName (i); schema::namesID (child, property) && ! child.hasType (schema::SEND))
                {
                    INFO ((child.getType().toString() + "@" + property.toString() + " " + child[property].toString()).toStdString());
                    CHECK (items.count (phazite::readID (child[property])) == 1);
                }
            }

            check (child);
        }
    };

    check (song);
}
} // namespace

TEST_CASE ("Two sides that changed different tracks merge with no clashes")
{
    Sides sides;
    phazite::setChannelVolume (*sides.ours, sides.tracks[0], -6.0f);
    phazite::setChannelVolume (*sides.theirs, sides.tracks[1], -9.0f);
    addMidiNote (*sides.theirs, sides.clip, 40, at (2.0), lasting (1.0));

    auto merged = sides.merge();
    CHECK (merged.clashes.empty());
    INFO (merged.notes.joinIntoString ("\n").toStdString());
    CHECK (merged.notes.isEmpty());
    CHECK ((float) findTrack (merged.song, "Bass")[schema::channelVolume] == doctest::Approx (-6.0f));
    CHECK ((float) findTrack (merged.song, "Drums")[schema::channelVolume] == doctest::Approx (-9.0f));

    // Their note is in this side's clip, beside this side's.
    const auto clip = findTrack (merged.song, "Bass").getChildWithName (schema::CLIP);
    int notes = 0;

    for (const auto& child : clip)
        notes += child.hasType (schema::NOTE) ? 1 : 0;

    CHECK (notes == 2);
    checkIDs (merged.song);
}

TEST_CASE ("A knob changed on both sides is a clash, this side's value kept, with all three")
{
    Sides sides;
    phazite::setChannelVolume (*sides.ours, sides.tracks[0], -6.0f);
    phazite::setChannelVolume (*sides.theirs, sides.tracks[0], -12.0f);

    auto merged = sides.merge();
    REQUIRE (merged.clashes.size() == 1);
    const auto& clash = merged.clashes.front();
    CHECK (clash.line == "Bass");
    CHECK (clash.what == "volume");
    CHECK (clash.property == schema::channelVolume);
    CHECK (clash.base.isVoid()); // the base had none: 0 dB
    CHECK ((float) clash.ours == doctest::Approx (-6.0f));
    CHECK ((float) clash.theirs == doctest::Approx (-12.0f));
    CHECK ((float) findTrack (merged.song, "Bass")[schema::channelVolume] == doctest::Approx (-6.0f));
    CHECK (phazite::findByPath (merged.song, clash.path) == findTrack (merged.song, "Bass"));

    // The same value on both sides is no clash.
    Sides same;
    phazite::setChannelVolume (*same.ours, same.tracks[0], -6.0f);
    phazite::setChannelVolume (*same.theirs, same.tracks[0], -6.0f);
    CHECK (same.merge().clashes.empty());
}

TEST_CASE ("Values set back to their defaults, one on each side, leave a device no values, as it was made")
{
    // The song keeps no value at its default, nor values without any (model/devices/internal/Parameters.h:
    // keepValueSetByHand()), and a merge leaves none either.
    Sides sides;
    const auto device = phazite::insertDeviceState (*sides.base, sides.tracks[1],
                                                    phazite::makeOwnDeviceState (*sides.base, phazite::devices::Synth::typeID));
    const auto defaultOf = [] (std::string_view parameter)
    {
        for (const auto& info : phazite::devices::Synth::info.parameters)
            if (info.id == parameter)
                return info.defaultValue;

        FAIL ("no parameter " << parameter);
        return 0.0f;
    };

    phazite::setValueSetByHand (*sides.base, { device, "cutoff" }, 800.0f);
    phazite::setValueSetByHand (*sides.base, { device, "resonance" }, 0.5f);
    sides.ours = phazite::makeSong (sides.base->getState().createCopy());
    sides.theirs = phazite::makeSong (sides.base->getState().createCopy());
    phazite::setValueSetByHand (*sides.ours, { device, "cutoff" }, defaultOf ("cutoff"));
    phazite::setValueSetByHand (*sides.theirs, { device, "resonance" }, defaultOf ("resonance"));
    REQUIRE (phazite::findDeviceState (*sides.ours, device).getChildWithName (schema::VALUES).getNumProperties() == 1);

    auto merged = sides.merge();
    CHECK (merged.clashes.empty());
    const auto song = phazite::makeSong (merged.song.createCopy());
    const auto state = phazite::findDeviceState (*song, device);
    REQUIRE (state.isValid());
    CHECK_FALSE (state.getChildWithName (schema::VALUES).isValid());
}

TEST_CASE ("What both sides added gets IDs of its own: tracks, devices, clips and notes never collide")
{
    Sides sides;

    // Both count on from the same place, so each side's first new track has the same ID.
    const auto ourTrack = phazite::insertTrack (*sides.ours, sides.tracks[1], nullptr);
    const auto theirTrack = phazite::insertTrack (*sides.theirs, sides.tracks[1], nullptr);
    REQUIRE (ourTrack == theirTrack);
    phazite::setTrackName (*sides.ours, ourTrack, "Keys");
    phazite::setTrackName (*sides.theirs, theirTrack, "Strings");
    phazite::insertDeviceState (*sides.ours, ourTrack, phazite::makeOwnDeviceState (*sides.ours, phazite::devices::Synth::typeID));
    phazite::insertDeviceState (*sides.theirs, theirTrack, phazite::makeOwnDeviceState (*sides.theirs, phazite::devices::Synth::typeID));
    addMidiNote (*sides.ours, sides.clip, 43, at (3.0), lasting (1.0));
    addMidiNote (*sides.theirs, sides.clip, 48, at (4.0), lasting (1.0));

    auto merged = sides.merge();
    CHECK (merged.clashes.empty());
    REQUIRE (findTrack (merged.song, "Keys").isValid());
    REQUIRE (findTrack (merged.song, "Strings").isValid());
    CHECK (findTrack (merged.song, "Keys")[schema::trackID] != findTrack (merged.song, "Strings")[schema::trackID]);
    checkIDs (merged.song);

    // Both notes in the clip, the first one too.
    int notes = 0;

    for (const auto& child : findTrack (merged.song, "Bass").getChildWithName (schema::CLIP))
        notes += child.hasType (schema::NOTE) ? 1 : 0;

    CHECK (notes == 3);
}

TEST_CASE ("Taken away on one side: it goes if the other left it as it was, and stays, with a note, if the other changed it")
{
    {
        Sides sides;
        phazite::deleteTracks (*sides.theirs, { sides.tracks[1] }, nullptr);
        CHECK_FALSE (findTrack (sides.merge().song, "Drums").isValid());
    }

    {
        Sides sides;
        phazite::deleteTracks (*sides.theirs, { sides.tracks[1] }, nullptr);
        phazite::setChannelVolume (*sides.ours, sides.tracks[1], -3.0f);
        const auto merged = sides.merge();
        CHECK (findTrack (merged.song, "Drums").isValid());
        CHECK (merged.notes.joinIntoString ("\n").contains ("Drums: changed here and taken away there: kept"));
    }

    // A clip taken away there and changed here: kept too.
    {
        Sides sides;
        phazite::deleteClips (*sides.theirs, { sides.clip }, false);
        addMidiNote (*sides.ours, sides.clip, 41, at (5.0), lasting (1.0));
        const auto merged = sides.merge();
        CHECK (findTrack (merged.song, "Bass").getChildWithName (schema::CLIP).isValid());
        CHECK (merged.notes.joinIntoString ("\n").contains ("Bass: clip \"Riff\" changed here and taken away there: kept"));
    }
}

TEST_CASE ("The other side's device order is taken where this side kept the base's, and its new device goes after what it followed")
{
    Sides sides;

    const auto addEq = [] (phazite::Song& song, phazite::TrackID track)
    { return phazite::insertDeviceState (song, track, phazite::makeOwnDeviceState (song, phazite::devices::Equalizer::typeID)); };

    const auto first = addEq (*sides.base, sides.tracks[0]);
    const auto second = addEq (*sides.base, sides.tracks[0]);
    sides.ours = phazite::makeSong (sides.base->getState().createCopy());
    sides.theirs = phazite::makeSong (sides.base->getState().createCopy());

    // Theirs: the two the other way round, and a third after the first.
    auto chain = phazite::findChainState (*sides.theirs, sides.tracks[0]);
    const auto firstState = phazite::findDeviceState (*sides.theirs, first);
    chain.moveChild (chain.indexOf (firstState), chain.getNumChildren() - 1, nullptr);
    const auto third = addEq (*sides.theirs, sides.tracks[0]);

    const auto merged = sides.merge();
    juce::Array<std::uint64_t> order;

    for (const auto& child : findTrack (merged.song, "Bass"))
        if (child.hasType (schema::DEVICE))
            order.add (phazite::readID (child[schema::deviceID]));

    REQUIRE (order.size() == 3);
    CHECK (order[0] == second.getRaw());
    CHECK (order[1] == first.getRaw());
    CHECK (order[2] == third.getRaw()); // which this side has nothing else with
    checkIDs (merged.song);
}

TEST_CASE ("Merging what this side has already changes nothing")
{
    Sides sides;
    phazite::setChannelVolume (*sides.ours, sides.tracks[0], -6.0f);
    addMidiNote (*sides.ours, sides.clip, 40, at (2.0), lasting (1.0));
    sides.theirs = phazite::makeSong (sides.ours->getState().createCopy());

    const auto merged = sides.merge();
    CHECK (merged.clashes.empty());
    CHECK (phazite::findChanges (sides.ours->getState(), merged.song).empty());
}

TEST_CASE ("Random edits on two sides always merge into a song whose IDs are each once, and whose references name what is there")
{
    juce::Random random (20261104);

    for (int round = 0; round < 12; ++round)
    {
        INFO ("round " << round);
        Sides sides;

        for (auto* side : { sides.ours.get(), sides.theirs.get() })
        {
            for (int edit = 0; edit < 8; ++edit)
            {
                const auto tracks = phazite::getTrackIDs (*side);
                const auto track = tracks[(size_t) random.nextInt ((int) tracks.size())];

                switch (random.nextInt (6))
                {
                    case 0:
                        phazite::setChannelVolume (*side, track, -random.nextFloat() * 30.0f);
                        break;
                    case 1:
                        phazite::insertTrack (*side, track, nullptr);
                        break;
                    case 2:
                        phazite::insertDeviceState (*side, track, phazite::makeOwnDeviceState (*side, phazite::devices::Equalizer::typeID));
                        break;
                    case 3:
                        if (const auto clip = addMidiClip (*side, track, { at (random.nextInt (16)), at (20.0) }); clip.isValid())
                            addMidiNote (*side, clip, 40 + random.nextInt (20), at (0.0), lasting (0.5));
                        break;
                    case 4:
                        if (phazite::findClipState (*side, sides.clip).isValid())
                            addMidiNote (*side, sides.clip, 30 + random.nextInt (40), at (random.nextInt (8)), lasting (0.25));
                        break;
                    case 5:
                    default:
                        if (tracks.size() > 1)
                            phazite::deleteTracks (*side, { track }, nullptr);
                        break;
                }
            }
        }

        const auto merged = sides.merge();
        checkIDs (merged.song);

        // It opens as a project's song, plays, and saves to open again as it was.
        TestSong s;
        s.openSong (merged.song);
        {
            EnginePlayback playback;
            phazite::playOrStop (*phazite::findSong (s.context));
            const auto audio = playback.play (0.25);
            phazite::playOrStop (*phazite::findSong (s.context));
            CHECK (std::isfinite (audio.getMagnitude (0, audio.getNumSamples())));
        }

        s.reopen();
        checkIDs (phazite::findSong (s.context)->getState());
        CHECK (phazite::getTrackIDs (*phazite::findSong (s.context)).size() == phazite::getTrackElements (merged.song).size());
    }
}

TEST_CASE ("A line merged again after a merge gave its additions new IDs merges onto the same items, through what the merge recorded")
{
    Sides sides;

    // Each side adds a track, both counting on from the same place: one ID for two tracks.
    phazite::ensureTracks (*sides.ours, 3);
    phazite::ensureTracks (*sides.theirs, 3);
    auto ourTrack = phazite::getTrackElements (sides.ours->getState()).back();
    auto theirTrack = phazite::getTrackElements (sides.theirs->getState()).back();
    REQUIRE (ourTrack[schema::trackID] == theirTrack[schema::trackID]);
    ourTrack.setProperty (schema::trackName, "Lead", nullptr);
    theirTrack.setProperty (schema::trackName, "Pad", nullptr);

    const auto first = sides.merge();
    REQUIRE_FALSE (first.renumbered.empty());
    const auto padID = findTrack (first.song, "Pad")[schema::trackID];
    CHECK (padID != ourTrack[schema::trackID]);

    // The other line goes on from what was merged in: its Pad renamed.
    const auto parted = sides.theirs->getState().createCopy();
    findTrack (sides.theirs->getState(), "Pad").setProperty (schema::trackName, "Pad 2", nullptr);

    // Merged again, the parting's and the other side's songs numbered as this side's through what the first merge recorded.
    auto base = parted.createCopy(), theirs = sides.theirs->getState().createCopy();
    phazite::translateIDs (base, first.renumbered);
    phazite::translateIDs (theirs, first.renumbered);
    CHECK (findTrack (theirs, "Pad 2")[schema::trackID] == padID);

    phazite::MergeLines lines;

    for (const auto& change : phazite::findChanges (base, theirs))
        if (change.where != phazite::SongChange::Where::song)
            lines.tracks.insert (change.trackID);

    auto ours = phazite::makeSong (first.song.createCopy());
    const auto second = phazite::mergeSongs (base, ours->getState(), theirs, lines, [&ours] { return ours->makeID(); });

    CHECK (second.clashes.empty());
    CHECK (findTrack (second.song, "Lead")[schema::trackID] == ourTrack[schema::trackID]);
    CHECK (findTrack (second.song, "Pad 2")[schema::trackID] == padID);
    CHECK_FALSE (findTrack (second.song, "Pad").isValid());
    CHECK (phazite::getTrackElements (second.song).size() == phazite::getTrackElements (first.song).size());
    checkIDs (second.song);
}

TEST_CASE ("The renumberings that apply are those of this line's merges since the parting that merged in the other line")
{
    std::map<juce::String, phazite::VersionRecord> versions;

    const auto add =
        [&versions] (const char* id, juce::int64 saved, juce::StringArray parents, std::vector<phazite::RenumberedID> renumbered = {})
    {
        phazite::VersionRecord record;
        record.id = id;
        record.saved = saved;
        record.parents = std::move (parents);
        record.renumbered = std::move (renumbered);
        versions.emplace (record.id, record);
    };

    // base, then ours o1 and theirs t1; m1 merges t1 into ours, renumbering 20 to 21; theirs goes on to t2; ours to o2. A
    // merge of another line (x1, into m2) renumbered too, which doesn't apply.
    add ("base", 1, {});
    add ("o1", 2, { "base" });
    add ("t1", 3, { "base" });
    add ("m1", 4, { "o1", "t1" }, { { {}, 20, 21 } });
    add ("x1", 5, { "base" });
    add ("m2", 6, { "m1", "x1" }, { { {}, 30, 31 } });
    add ("t2", 7, { "t1" });

    const auto parted = phazite::findMergeBase (versions, "m2", "t2");
    CHECK (parted == "t1");

    const auto renumbering = phazite::findRenumbering (versions, "m2", "t2", parted);
    REQUIRE (renumbering.size() == 1);
    CHECK (renumbering[0].from == 20);
    CHECK (renumbering[0].to == 21);
}

TEST_CASE ("A drum sampler a merge gives a new ID keeps its pads' tracks, which name it by the new one")
{
    Sides sides;

    // Each side adds a device, both counting on from the same place: one ID for this side's synth and the other side's drum
    // sampler, whose pad 36 the other side gave a track of its own.
    const auto synth =
        phazite::setInstrument (*sides.ours, sides.tracks[0], phazite::makeOwnDeviceState (*sides.ours, phazite::devices::Synth::typeID));
    const auto drums = phazite::setInstrument (*sides.theirs, sides.tracks[1],
                                               phazite::makeOwnDeviceState (*sides.theirs, phazite::devices::DrumSampler::typeID));
    REQUIRE (synth.isValid());
    REQUIRE (drums.isValid());
    REQUIRE (synth.getRaw() == drums.getRaw());
    REQUIRE (phazite::givePadItsOwnTrack (*sides.theirs, drums, 36).isValid());

    const auto merged = sides.merge();
    CHECK (merged.clashes.empty());

    // The drum sampler, given a new ID, and the pad's track, which names it by that one, not by the synth's.
    juce::ValueTree sampler, padTrack;
    const std::function<void (const juce::ValueTree&)> find = [&] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (schema::DEVICE) && child[schema::deviceType].toString() == phazite::devices::DrumSampler::typeID)
                sampler = child;

            if (child.hasType (schema::TRACK) && child.hasProperty (schema::trackPadDevice))
                padTrack = child;

            find (child);
        }
    };

    find (merged.song);
    REQUIRE (sampler.isValid());
    REQUIRE (padTrack.isValid());
    CHECK (phazite::readID (sampler[schema::deviceID]) != drums.getRaw());
    CHECK (phazite::readID (padTrack[schema::trackPadDevice]) == phazite::readID (sampler[schema::deviceID]));
    CHECK ((int) padTrack[schema::trackPad] == 36);
    checkIDs (merged.song);
}
