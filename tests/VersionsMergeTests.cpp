// SPDX-License-Identifier: AGPL-3.0-or-later
// Merging two lines of a song by IDs (docs/design/versions.md: Merging by IDs, step V4): each side a song of its own made from the
// same base, changed through the model, and the other side's lines taken into this side's.
#include "EnginePlayback.h"
#include "RandomEdits.h"
#include "SongCheck.h"
#include "SongIDs.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/drums/DrumSampler.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/VersionHistory.h"

#include <map>
#include <optional>
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
        ours = phazite::makeSong (phazite::test::copyWholeSong (*base));
        theirs = phazite::makeSong (phazite::test::copyWholeSong (*base));
    }

    // The other side's changes, every line of them (a merge takes them all), merged into this side's; and what that merge
    // makes of this side's song holds together, as every song must (SongCheck.h, the alias rule with it), each case's.
    phazite::MergeResult merge()
    {
        auto merged = phazite::mergeSongs (base->getState(), ours->getState(), theirs->getState(), [this] { return ours->makeID(); });
        checkMerged (merged);
        return merged;
    }

    // The merge applied to this side's song, as Merge applies it (VersionsModel.cpp): one step, what names the gone taken away.
    static void apply (phazite::Song& song, const phazite::MergeResult& merged)
    {
        const phazite::UndoStep step (song, "Merge");
        phazite::makeSongLike (song, merged.song);
        phazite::removeBrokenReferences (song);
    }

    phazite::MergeResult mergeIntoOurs()
    {
        auto merged = merge();
        apply (*ours, merged);
        return merged;
    }

    // A merge applied to a copy of this side's song, which then holds together, as it opens (copyAsOpened(): without the
    // curves of devices taken away, which an edited song keeps for an undo).
    void checkMerged (const phazite::MergeResult& merged) const
    {
        INFO ("the merged song");
        const auto applied = phazite::makeSong (phazite::test::copyWholeSong (*ours));
        REQUIRE (applied != nullptr);
        apply (*applied, merged);
        checkSong (*copyAsOpened (*applied));
    }
};

juce::ValueTree findTrack (const juce::ValueTree& song, const juce::String& name)
{
    for (const auto& track : phazite::getTrackElements (song))
        if (track[schema::trackName].toString() == name)
            return track;

    return {};
}

// The elements of a type and ID anywhere in a song: one, for what the song holds once.
std::vector<juce::ValueTree> findItems (const juce::ValueTree& song, const juce::Identifier& type, std::uint64_t id)
{
    std::vector<juce::ValueTree> found;
    const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (type) && phazite::readID (child[schema::trackID]) == id)
                found.push_back (child);

            visit (child);
        }
    };

    visit (song);
    return found;
}

int countNotes (const juce::ValueTree& clip)
{
    int notes = 0;

    for (const auto& child : clip)
        notes += child.hasType (schema::NOTE) ? 1 : 0;

    return notes;
}

// The base's two tracks put into a folder named Group, and both sides made from it again.
phazite::TrackID groupBase (Sides& sides)
{
    const auto folder = phazite::groupIntoFolder (*sides.base, sides.tracks, nullptr);
    phazite::setTrackName (*sides.base, folder, "Group");
    sides.ours = phazite::makeSong (phazite::test::copyWholeSong (*sides.base));
    sides.theirs = phazite::makeSong (phazite::test::copyWholeSong (*sides.base));
    return folder;
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
    sides.ours = phazite::makeSong (phazite::test::copyWholeSong (*sides.base));
    sides.theirs = phazite::makeSong (phazite::test::copyWholeSong (*sides.base));
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

TEST_CASE ("A macro the other side added and linked to a MIDI controller keeps its link as the merge gives it a new ID")
{
    Sides sides;

    // Both count on from the same place, so each side's first macro has the same ID, and theirs is given another.
    const auto ourMacro = phazite::addModulator (*sides.ours, sides.tracks[0], phazite::ModulatorType::macro);
    const auto theirMacro = phazite::addModulator (*sides.theirs, sides.tracks[1], phazite::ModulatorType::macro);
    REQUIRE (ourMacro == theirMacro);
    const phazite::ParameterRef macro { phazite::DeviceID::fromRaw (theirMacro.getRaw()), juce::String (theirMacro.getRaw()) };
    REQUIRE (phazite::describeParameter (*sides.theirs, macro).has_value());
    phazite::linkMidiController (*sides.theirs, { 1, 1 }, macro);

    CHECK_FALSE (sides.mergeIntoOurs().renumbered.empty());
    const auto linked = phazite::findLinkedParameter (*sides.ours, { 1, 1 });
    REQUIRE (linked.has_value());
    CHECK (linked->device != macro.device);
    CHECK (phazite::describeParameter (*sides.ours, *linked).has_value());
    const auto state = phazite::findModulatorState (*sides.ours, phazite::ModulatorID::fromRaw (linked->device.getRaw()));
    CHECK (state.hasType (schema::MACRO));
    CHECK (state.getParent()[schema::trackName].toString() == "Drums");
    checkSong (*sides.ours);
}

TEST_CASE ("A merge keeps one link of each MIDI controller and one of each parameter, each link whole")
{
    Sides sides;
    const auto synthA = phazite::insertDeviceState (*sides.base, sides.tracks[0],
                                                    phazite::makeOwnDeviceState (*sides.base, phazite::devices::Synth::typeID));
    const auto synthB = phazite::insertDeviceState (*sides.base, sides.tracks[1],
                                                    phazite::makeOwnDeviceState (*sides.base, phazite::devices::Synth::typeID));
    const phazite::ParameterRef cutoffA { synthA, "cutoff" }, resonanceA { synthA, "resonance" }, cutoffB { synthB, "cutoff" };
    phazite::linkMidiController (*sides.base, { 1, 1 }, cutoffA);
    sides.ours = phazite::makeSong (phazite::test::copyWholeSong (*sides.base));
    sides.theirs = phazite::makeSong (phazite::test::copyWholeSong (*sides.base));

    SUBCASE ("Both sides link another controller to the same parameter: this side's link stays")
    {
        phazite::linkMidiController (*sides.ours, { 1, 2 }, resonanceA);
        phazite::linkMidiController (*sides.theirs, { 1, 3 }, resonanceA);
        sides.mergeIntoOurs();
        CHECK (phazite::findMidiLink (*sides.ours, resonanceA) == phazite::MidiController { 1, 2 });
        CHECK_FALSE (phazite::findLinkedParameter (*sides.ours, { 1, 3 }).has_value());
    }

    // This side moved the link to another device's knob, the other to another knob of the same device: never another
    // device's knob of the one, the other's knob of the other.
    SUBCASE ("Both sides link the same controller to different parameters: one side's link, whole")
    {
        phazite::linkMidiController (*sides.ours, { 1, 1 }, cutoffB);
        phazite::linkMidiController (*sides.theirs, { 1, 1 }, resonanceA);
        const auto merged = sides.mergeIntoOurs();
        CHECK (phazite::findLinkedParameter (*sides.ours, { 1, 1 }) == cutoffB);
        CHECK (merged.clashes.size() == 1);
    }

    checkSong (*sides.ours);
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
    sides.ours = phazite::makeSong (phazite::test::copyWholeSong (*sides.base));
    sides.theirs = phazite::makeSong (phazite::test::copyWholeSong (*sides.base));

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
    sides.theirs = phazite::makeSong (phazite::test::copyWholeSong (*sides.ours));

    const auto merged = sides.merge();
    CHECK (merged.clashes.empty());
    CHECK (phazite::findChanges (sides.ours->getState(), merged.song).empty());
}

TEST_CASE ("Random edits on two sides always merge into a song whose IDs are each once, and whose references name what is there")
{
    // Every kind of edit the model has (RandomEdits.h), round r's sides from the seeds 20261104 + 2r and the one after.
    TemporaryFolder sounds;

    // The tracks and the clips of a song, by their IDs.
    const auto itemsOf = [] (const juce::ValueTree& song)
    {
        std::set<std::pair<juce::String, std::uint64_t>> items;

        for (const auto& track : phazite::getTrackElements (song))
        {
            items.insert ({ "track", phazite::readID (track[schema::trackID]) });

            for (const auto& child : track)
                if (child.hasType (schema::CLIP))
                    items.insert ({ "clip", phazite::readID (child[schema::clipID]) });
        }

        return items;
    };

    for (int round = 0; round < 80; ++round)
    {
        INFO ("round " << round);
        Sides sides;

        // The base with a folder, in half of the rounds.
        if (round % 2 == 1)
            groupBase (sides);

        for (int side = 0; side < 2; ++side)
        {
            const auto seed = 20261104 + 2 * round + side;
            INFO ("side " << side << ", seed " << seed);
            RandomEdits (side == 0 ? *sides.ours : *sides.theirs, seed, sounds.folder).edit (8);
            // Each side's alias groups as the model made them hold together (the rest of the check is for a song as it opens:
            // an edited song keeps the curves of devices gone for an undo).
            for (const auto& problem : findSongProblems (side == 0 ? *sides.ours : *sides.theirs))
            {
                INFO (problem);
                CHECK (problem.rfind ("alias", 0) != 0);
            }
        }

        const auto merged = sides.merge();
        checkIDs (merged.song);

        // What all three have, neither side took away, and what this side added, it has still: a track ungrouped, a clip moved.
        const auto inBase = itemsOf (sides.base->getState()), inOurs = itemsOf (sides.ours->getState()),
                   inTheirs = itemsOf (sides.theirs->getState()), inMerged = itemsOf (merged.song);

        for (const auto& item : inOurs)
        {
            if ((inBase.count (item) > 0) == (inTheirs.count (item) > 0))
            {
                INFO ("lost: " << item.first.toStdString() << " " << item.second);
                CHECK (inMerged.count (item) == 1);
            }
        }

        if (round >= 12)
            continue;

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
    const auto parted = phazite::test::copyWholeSong (*sides.theirs);
    findTrack (sides.theirs->getState(), "Pad").setProperty (schema::trackName, "Pad 2", nullptr);

    // Merged again, the parting's and the other side's songs numbered as this side's through what the first merge recorded.
    auto base = parted.createCopy(), theirs = phazite::test::copyWholeSong (*sides.theirs);
    phazite::translateIDs (base, first.renumbered);
    phazite::translateIDs (theirs, first.renumbered);
    CHECK (findTrack (theirs, "Pad 2")[schema::trackID] == padID);

    auto ours = phazite::makeSong (first.song.createCopy());
    const auto second = phazite::mergeSongs (base, ours->getState(), theirs, [&ours] { return ours->makeID(); });

    CHECK (second.clashes.empty());
    CHECK (findTrack (second.song, "Lead")[schema::trackID] == ourTrack[schema::trackID]);
    CHECK (findTrack (second.song, "Pad 2")[schema::trackID] == padID);
    CHECK_FALSE (findTrack (second.song, "Pad").isValid());
    CHECK (phazite::getTrackElements (second.song).size() == phazite::getTrackElements (first.song).size());
    checkIDs (second.song);
}

TEST_CASE ("A note the other line made with the ID a merge gave one of its notes in that clip gets one of its own, and a record "
           "that isn't one to one is left out")
{
    // The parting's clip holds the other line's note n; a merge here gave it the ID t; their line then made a note of its
    // own, with t, in the same clip.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 1);
    const auto clip = addMidiClip (*song, tracks[0], { at (0.0), at (4.0) }, "Riff");
    addMidiNote (*song, clip, 36, at (0.0), lasting (1.0));
    const auto base = phazite::test::copyWholeSong (*song);
    auto theirs = base.createCopy();
    REQUIRE (findItems (theirs, schema::CLIP, clip.getRaw()).size() == 1);
    auto theirClip = findItems (theirs, schema::CLIP, clip.getRaw()).front();
    const auto note = theirClip.getChildWithName (schema::NOTE);
    REQUIRE (note.isValid());
    const auto n = phazite::readID (note[schema::noteID]);
    const std::uint64_t t = n + 100;
    auto own = note.createCopy();
    own.setProperty (schema::noteID, (juce::int64) t, nullptr);
    own.setProperty (schema::notePitch, 40, nullptr);
    theirClip.appendChild (own, nullptr);

    const auto group = "clip " + juce::String ((juce::uint64) clip.getRaw());
    std::uint64_t next = 5000;
    const auto translation = phazite::findTranslation (base, theirs, { { group, n, t } }, [&next] { return next++; });
    REQUIRE (translation.givenAnew.size() == 1);
    CHECK (translation.givenAnew[0] == phazite::RenumberedID { group, t, 5000 });

    auto translatedBase = base.createCopy(), translatedTheirs = theirs.createCopy();
    phazite::translateIDs (translatedBase, translation.all);
    phazite::translateIDs (translatedTheirs, translation.all);
    std::set<std::uint64_t> ids;

    const auto translatedClip = findItems (translatedTheirs, schema::CLIP, clip.getRaw()).front();

    for (const auto& each : translatedClip)
        if (each.hasType (schema::NOTE))
            CHECK (ids.insert (phazite::readID (each[schema::noteID])).second);

    CHECK (ids == std::set<std::uint64_t> { t, 5000 });
    CHECK (phazite::readID (findItems (translatedBase, schema::CLIP, clip.getRaw()).front().getChildWithName (schema::NOTE)[schema::noteID])
           == t);

    // Two IDs given one, or an ID 0, as no merge records: none of them translates anything.
    const auto bass = tracks[0].getRaw();
    const auto broken = phazite::findTranslation (base, base, { { {}, bass, bass + 50 }, { {}, clip.getRaw(), bass + 50 }, { {}, 0, 7 } },
                                                  [&next] { return next++; });
    CHECK (broken.all.empty());
    CHECK (broken.givenAnew.empty());
    CHECK (phazite::areOneToOne ({ { {}, 1, 2 }, { group, 1, 2 } }));
    CHECK_FALSE (phazite::areOneToOne ({ { {}, 1, 2 }, { {}, 3, 2 } }));
    CHECK_FALSE (phazite::areOneToOne ({ { {}, 0, 2 } }));
}

TEST_CASE ("The other line's merges of a version both merged cross to this line's IDs: the latest word, for what its song holds, onto "
           "an ID it holds only where that crosses away too, and its own item under such an ID given one of its own")
{
    // The review of S4.5: three lines that merged the same version, a note crossed onto a note their clip holds that crosses
    // away itself (known bug 199), and the other line's own item under an ID its merge at the parting gave anew, which this
    // line's records would take for the parting's.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 3);
    const auto clip = addMidiClip (*song, tracks[0], { at (0.0), at (4.0) }, "Riff");
    addMidiNote (*song, clip, 36, at (0.0), lasting (1.0));
    addMidiNote (*song, clip, 38, at (1.0), lasting (1.0));
    const auto theirs = phazite::test::copyWholeSong (*song);
    std::vector<std::uint64_t> notes;

    const auto clips = findItems (theirs, schema::CLIP, clip.getRaw());
    REQUIRE (clips.size() == 1);

    for (const auto& note : clips.front())
        if (note.hasType (schema::NOTE))
            notes.push_back (phazite::readID (note[schema::noteID]));

    REQUIRE (notes.size() == 2);
    const auto group = "clip " + juce::String ((juce::uint64) clip.getRaw());
    const auto t0 = tracks[0].getRaw(), t1 = tracks[1].getRaw(), t2 = tracks[2].getRaw();
    constexpr std::uint64_t far = 1000000; // no ID the song holds

    const std::vector<phazite::RenumberedID> theirMerges {
        { {}, far + 900, far + 901 },  // what their song no longer holds: nothing
        { {}, far + 10, far + 902 },   // an earlier merge's word...
        { {}, far + 10, t1 },          // ...and the latest's, which their song holds
        { group, far + 20, notes[0] }, // onto a note their clip holds, which crosses away
        { group, far + 30, notes[1] }, // a note crossed
        { {}, t2, t0 },                // an item crossed, and their own Lead under t2, which the parting holds
    };
    const std::vector<phazite::RenumberedID> ourMerges {
        { group, far + 20, notes[1] },
        { group, far + 30, far + 31 },
        { {}, t2, far + 41 },
        { group, t2, far + 77 }, // a note's word, which says nothing of the item
    };

    std::uint64_t next = 5000;
    // Their merge of t2 at the parting, which holds the version's item under t2.
    auto crossed = phazite::crossRenumbering (theirMerges, ourMerges, { { {}, t2, t0 } }, {}, theirs, theirs, [&next] { return next++; });
    std::sort (crossed.begin(), crossed.end(),
               [] (const auto& a, const auto& b) { return a.clip != b.clip ? a.clip.compare (b.clip) < 0 : a.from < b.from; });

    const std::vector<phazite::RenumberedID> expected {
        { {}, t0, far + 41 }, { {}, t1, far + 10 }, { {}, t2, 5000 }, { group, notes[0], notes[1] }, { group, notes[1], far + 31 },
    };
    CHECK (crossed == expected);

    // Their own item under t2 given none where the parting hasn't t2 (nothing translates it there).
    auto without = theirs.createCopy();
    const auto lead = findItems (without, schema::TRACK, t2);
    REQUIRE (lead.size() == 1);
    juce::ValueTree (lead.front()).getParent().removeChild (lead.front(), nullptr);
    CHECK (phazite::crossRenumbering ({ { {}, t2, t0 } }, { { {}, t2, far + 41 } }, { { {}, t2, t0 } }, {}, without, theirs,
                                      [&next] { return next++; })
           == std::vector<phazite::RenumberedID> { { {}, t0, far + 41 } });
}

TEST_CASE ("Crossings that aren't one to one are left out, and the other line's own item still gets an ID of its own")
{
    // The review of S4.5: records another history wrote that cross two items to one ID dropped every entry, the new ID of the
    // other line's own item too, which then was taken for the parting's item under that ID.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 3);
    const auto theirs = phazite::test::copyWholeSong (*song);
    const auto t0 = tracks[0].getRaw(), t1 = tracks[1].getRaw(), t2 = tracks[2].getRaw();
    constexpr std::uint64_t far = 1000000; // no ID the song holds

    const std::vector<phazite::RenumberedID> theirMerges { { {}, far + 10, t0 }, { {}, far + 11, t1 }, { {}, t2, far + 60 } };
    const std::vector<phazite::RenumberedID> ourMerges { { {}, far + 10, far + 50 }, { {}, far + 11, far + 50 } }; // two to one
    std::uint64_t next = 5000;
    CHECK (phazite::crossRenumbering (theirMerges, ourMerges, { { {}, t2, far + 60 } }, {}, theirs, theirs, [&next] { return next++; })
           == std::vector<phazite::RenumberedID> { { {}, t2, 5000 } });
}

TEST_CASE ("The other line's song crosses into the parting's numbering where this line's records at the parting take an item on, so "
           "that nothing is translated twice (known bug 199)")
{
    // The parting holds Pad under a and Rhodes under b; this line's merges took them to b and c.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 4);
    const auto a = tracks[0].getRaw(), b = tracks[1].getRaw(), c = tracks[2].getRaw(), d = tracks[3].getRaw();

    const auto songOf = [&song] (const std::map<std::uint64_t, juce::String>& names)
    {
        auto copy = phazite::test::copyWholeSong (*song);

        for (auto track : phazite::getTrackElements (copy))
        {
            if (const auto found = names.find (phazite::readID (track[schema::trackID])); found != names.end())
                track.setProperty (schema::trackName, found->second, nullptr);
            else
                track.getParent().removeChild (track, nullptr);
        }

        return copy;
    };

    const auto base = songOf ({ { a, "Pad" }, { b, "Rhodes" } });
    const std::vector<phazite::RenumberedID> ours { { {}, a, b }, { {}, b, c } };

    // Both stages, as a merge takes them: the other line's song, each track's ID by its name, and what the merge records.
    struct Merged
    {
        std::vector<phazite::RenumberedID> crossed, recorded;
        std::map<juce::String, std::uint64_t> theirs;
    };

    const auto merge = [&base, &ours] (const juce::ValueTree& theirsRead, const std::vector<phazite::RenumberedID>& theirMerges)
    {
        std::uint64_t next = 5000;
        const auto makeID = [&next] { return next++; };
        Merged merged;
        merged.crossed = phazite::crossRenumbering (theirMerges, ours, theirMerges, ours, base, theirsRead, makeID);
        auto theirs = theirsRead.createCopy(), parting = base.createCopy();
        phazite::translateIDs (theirs, merged.crossed);
        const auto translation = phazite::findTranslation (parting, theirs, ours, makeID, merged.crossed);
        phazite::translateIDs (parting, translation.all);
        phazite::translateIDs (theirs, translation.all);
        CHECK (translation.givenAnew.empty());
        CHECK (phazite::holdsEachIDOnce (theirs));
        merged.recorded = phazite::recordCrossings (merged.crossed, translation.all);

        for (const auto& track : phazite::getTrackElements (theirs))
            merged.theirs[track[schema::trackName].toString()] = phazite::readID (track[schema::trackID]);

        for (auto* ids : { &merged.crossed, &merged.recorded })
            std::sort (ids->begin(), ids->end(), [] (const auto& x, const auto& y) { return x.from < y.from; });

        return merged;
    };

    SUBCASE ("the other line's merges took them to b and c too, and its Keys holds a")
    {
        // Pad to a, Rhodes to b, Keys out of the way first; then this line's records take Pad to b and Rhodes to c, once each.
        const auto merged = merge (songOf ({ { a, "Keys" }, { b, "Pad" }, { c, "Rhodes" } }), ours);
        CHECK (merged.crossed == std::vector<phazite::RenumberedID> { { {}, a, 5000 }, { {}, b, a }, { {}, c, b } });
        CHECK (merged.theirs == std::map<juce::String, std::uint64_t> { { "Keys", 5000 }, { "Pad", b }, { "Rhodes", c } });
        CHECK (merged.recorded == std::vector<phazite::RenumberedID> { { {}, a, 5000 }, { {}, b, b }, { {}, c, c } });
    }

    SUBCASE ("the other line's merge took Pad to d alone, and it holds the parting's Rhodes under b, as the parting does")
    {
        // Rhodes is the parting's, which this line's records take to c, not an item of that line's in the way of Pad.
        const auto merged = merge (songOf ({ { a, "Keys" }, { b, "Rhodes" }, { d, "Pad" } }), { { {}, a, d } });
        CHECK (merged.crossed == std::vector<phazite::RenumberedID> { { {}, a, 5000 }, { {}, d, a } });
        CHECK (merged.theirs == std::map<juce::String, std::uint64_t> { { "Keys", 5000 }, { "Pad", b }, { "Rhodes", c } });
        CHECK (merged.recorded == std::vector<phazite::RenumberedID> { { {}, a, 5000 }, { {}, d, b } });
    }
}

TEST_CASE ("The other line's own note under an ID its merge gave away is found by its clip as the parting numbers it, where both "
           "lines' merges gave that clip new IDs (known bug 199)")
{
    // The parting's clip k holds note n. This line's merge took the clip to k2; the other line's to k3, and gave the parting's
    // note there a new ID (its own note held n), then took that note out. Its own note n is no note of the parting's.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 1);
    const auto clip = addMidiClip (*song, tracks[0], { at (0.0), at (4.0) }, "Riff");
    addMidiNote (*song, clip, 36, at (0.0), lasting (1.0));
    const auto base = phazite::test::copyWholeSong (*song);
    constexpr std::uint64_t far = 1000000; // no ID either song holds
    const auto k = clip.getRaw(), k2 = far + 2, k3 = far + 3;

    auto theirs = base.createCopy();
    auto theirClip = findItems (theirs, schema::CLIP, k).front();
    theirClip.setProperty (schema::clipID, (juce::int64) k3, nullptr);
    const auto n = phazite::readID (theirClip.getChildWithName (schema::NOTE)[schema::noteID]);
    const auto group = [] (std::uint64_t id) { return "clip " + juce::String ((juce::uint64) id); };

    const std::vector<phazite::RenumberedID> theirMerges { { {}, k, k3 }, { group (k3), n, far + 5 } };
    const std::vector<phazite::RenumberedID> ours { { {}, k, k2 } };
    std::uint64_t next = 5000;
    const auto makeID = [&next] { return next++; };
    auto crossed = phazite::crossRenumbering (theirMerges, ours, theirMerges, ours, base, theirs, makeID);
    std::sort (crossed.begin(), crossed.end(), [] (const auto& a, const auto& b) { return a.clip.compare (b.clip) < 0; });
    CHECK (crossed == std::vector<phazite::RenumberedID> { { {}, k3, k }, { group (k), n, 5000 } });

    // Translated on: the clip is this line's k2 on both sides, and the other line's note an addition of its own there.
    auto translatedBase = base.createCopy();
    phazite::translateIDs (theirs, crossed);
    const auto translation = phazite::findTranslation (translatedBase, theirs, ours, makeID, crossed);
    phazite::translateIDs (translatedBase, translation.all);
    phazite::translateIDs (theirs, translation.all);
    const auto ids = [] (const juce::ValueTree& tree, std::uint64_t clipID)
    {
        const auto found = findItems (tree, schema::CLIP, clipID);
        REQUIRE (found.size() == 1);
        return phazite::readID (found.front().getChildWithName (schema::NOTE)[schema::noteID]);
    };
    CHECK (ids (translatedBase, k2) == n);
    CHECK (ids (theirs, k2) == 5000);
}

TEST_CASE ("What crossed is recorded by the IDs the merged song holds, a note by its clip as that numbers it")
{
    // An item the second stage takes on, a note in a clip it gives another ID, and a note neither touches.
    const std::vector<phazite::RenumberedID> crossed { { {}, 10, 20 }, { "clip 20", 3, 4 }, { "clip 7", 1, 2 } };
    const std::vector<phazite::RenumberedID> translation { { {}, 20, 30 }, { "clip 30", 4, 8 } };
    CHECK (phazite::recordCrossings (crossed, translation)
           == std::vector<phazite::RenumberedID> { { {}, 10, 30 }, { "clip 30", 3, 8 }, { "clip 7", 1, 2 } });
    CHECK (phazite::recordCrossings (crossed, {}) == crossed);
}

TEST_CASE ("A song holds each ID once where each item's is once, and each note's once in its clip")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto first = addMidiClip (*song, tracks[0], { at (0.0), at (4.0) }, "Riff");
    const auto second = addMidiClip (*song, tracks[1], { at (0.0), at (4.0) }, "Riff");
    addMidiNote (*song, first, 36, at (0.0), lasting (1.0));
    addMidiNote (*song, first, 38, at (1.0), lasting (1.0));
    addMidiNote (*song, second, 40, at (0.0), lasting (1.0));
    CHECK (phazite::holdsEachIDOnce (song->getState()));

    const auto notesOf = [] (const juce::ValueTree& tree, phazite::ClipID clip)
    {
        const auto clips = findItems (tree, schema::CLIP, clip.getRaw());
        REQUIRE (clips.size() == 1);
        std::vector<juce::ValueTree> notes;

        for (const auto& note : clips.front())
            if (note.hasType (schema::NOTE))
                notes.push_back (note);

        return notes;
    };

    // A note with the ID of a note in another clip: once in each.
    auto acrossClips = phazite::test::copyWholeSong (*song);
    juce::ValueTree (notesOf (acrossClips, second)[0])
        .setProperty (schema::noteID, notesOf (acrossClips, first)[0][schema::noteID], nullptr);
    CHECK (phazite::holdsEachIDOnce (acrossClips));

    // Two notes of one clip, or two tracks, under one ID: twice.
    auto inOneClip = phazite::test::copyWholeSong (*song);
    const auto notes = notesOf (inOneClip, first);
    REQUIRE (notes.size() == 2);
    juce::ValueTree (notes[1]).setProperty (schema::noteID, notes[0][schema::noteID], nullptr);
    CHECK_FALSE (phazite::holdsEachIDOnce (inOneClip));

    auto twoTracks = phazite::test::copyWholeSong (*song);
    auto trackElements = phazite::getTrackElements (twoTracks);
    REQUIRE (trackElements.size() == 2);
    trackElements[1].setProperty (schema::trackID, trackElements[0][schema::trackID], nullptr);
    CHECK_FALSE (phazite::holdsEachIDOnce (twoTracks));
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

TEST_CASE (
    "A folder taken away there goes without its tracks: ungrouped there, they stay, out of it; deleted with it, those changed here stay")
{
    // Ungrouped there: the folder goes, and its tracks are where the other side has them.
    {
        Sides sides;
        const auto folder = groupBase (sides);
        phazite::ungroupFolder (*sides.theirs, folder);

        const auto merged = sides.merge();
        checkIDs (merged.song);
        CHECK (merged.clashes.empty());
        CHECK_FALSE (findTrack (merged.song, "Group").isValid());
        REQUIRE (findTrack (merged.song, "Bass").isValid());
        REQUIRE (findTrack (merged.song, "Drums").isValid());
        CHECK (findTrack (merged.song, "Bass").getParent().hasType (schema::TRACKS));
        CHECK (findTrack (merged.song, "Bass").getChildWithName (schema::CLIP).isValid());
        CHECK (phazite::findChanges (sides.theirs->getState(), merged.song).empty());
    }

    // Ungrouped there, with its tracks the other way round, while this side changed one of them.
    {
        Sides sides;
        const auto folder = groupBase (sides);
        phazite::ungroupFolder (*sides.theirs, folder);
        phazite::moveTracksAfter (*sides.theirs, { sides.tracks[0] }, sides.tracks[1]);
        phazite::setChannelVolume (*sides.ours, sides.tracks[0], -6.0f);

        const auto merged = sides.merge();
        checkIDs (merged.song);
        const auto tracks = phazite::getTrackElements (merged.song);
        REQUIRE (tracks.size() == 2);
        CHECK (tracks[0][schema::trackName].toString() == "Drums");
        CHECK (tracks[1][schema::trackName].toString() == "Bass");
        CHECK ((float) tracks[1][schema::channelVolume] == doctest::Approx (-6.0f));
    }

    // Deleted there with its tracks: they go with it, but the one that changed here, which stays, with a note.
    {
        Sides sides;
        const auto folder = groupBase (sides);
        phazite::deleteTracks (*sides.theirs, { folder }, nullptr);
        phazite::setChannelVolume (*sides.ours, sides.tracks[0], -6.0f);

        const auto merged = sides.merge();
        checkIDs (merged.song);
        CHECK_FALSE (findTrack (merged.song, "Group").isValid());
        CHECK_FALSE (findTrack (merged.song, "Drums").isValid());
        REQUIRE (findTrack (merged.song, "Bass").isValid());
        CHECK (findTrack (merged.song, "Bass").getParent().hasType (schema::TRACKS));
        CHECK (merged.notes.joinIntoString ("\n").contains ("Bass: changed here and taken away there: kept"));
    }
}

TEST_CASE ("A folder ungrouped here and changed there comes back without the tracks this side took out of it, each of them once")
{
    Sides sides;
    const auto folder = groupBase (sides);
    phazite::ungroupFolder (*sides.ours, folder);
    phazite::setTrackName (*sides.theirs, folder, "Rhythm");

    const auto merged = sides.merge();
    checkIDs (merged.song);
    CHECK (phazite::getTrackElements (merged.song).size() == 3);
    REQUIRE (findTrack (merged.song, "Rhythm").isValid());
    REQUIRE (findTrack (merged.song, "Bass").isValid());
    CHECK (findTrack (merged.song, "Bass").getParent().hasType (schema::TRACKS));
    CHECK (findTrack (merged.song, "Drums").getParent().hasType (schema::TRACKS));
    CHECK (merged.notes.joinIntoString ("\n").contains ("Rhythm: taken away here and changed there: kept"));
}

TEST_CASE ("A track added there in a folder added there is in the folder here, with the track the other side moved into it")
{
    Sides sides;
    const auto keys = phazite::insertTrack (*sides.theirs, sides.tracks[1], nullptr);
    phazite::setTrackName (*sides.theirs, keys, "Keys");
    const auto folder = phazite::groupIntoFolder (*sides.theirs, { sides.tracks[1], keys }, nullptr);
    phazite::setTrackName (*sides.theirs, folder, "Group");
    REQUIRE (keys.getRaw() < folder.getRaw()); // the track's line before its folder's, by their IDs

    const auto merged = sides.merge();
    checkIDs (merged.song);
    REQUIRE (findTrack (merged.song, "Group").isValid());
    REQUIRE (findTrack (merged.song, "Keys").isValid());
    CHECK (findTrack (merged.song, "Keys").getParent() == findTrack (merged.song, "Group"));
    CHECK (findTrack (merged.song, "Drums").getParent() == findTrack (merged.song, "Group"));
    CHECK (phazite::findChanges (sides.theirs->getState(), merged.song).empty());
}

TEST_CASE ("A clip moved onto another track on one side and changed on the other is once in the song, where it was moved to, as it changed")
{
    const auto clipOf = [] (const juce::ValueTree& song, const juce::String& track)
    { return findTrack (song, track).getChildWithName (schema::CLIP); };

    // Moved there, changed here.
    {
        Sides sides;
        phazite::moveClip (*sides.theirs, sides.clip, at (4.0), sides.tracks[1]);
        addMidiNote (*sides.ours, sides.clip, 40, at (2.0), lasting (1.0));

        const auto merged = sides.merge();
        checkIDs (merged.song);
        CHECK (merged.clashes.empty());
        CHECK (findItems (merged.song, schema::CLIP, sides.clip.getRaw()).size() == 1);
        CHECK_FALSE (clipOf (merged.song, "Bass").isValid());
        REQUIRE (clipOf (merged.song, "Drums").isValid());
        CHECK (countNotes (clipOf (merged.song, "Drums")) == 2);
        CHECK ((double) clipOf (merged.song, "Drums")[schema::clipStart] == doctest::Approx (4.0));
        CHECK_FALSE (merged.notes.joinIntoString ("\n").contains ("taken away"));
    }

    // Moved here, changed there.
    {
        Sides sides;
        phazite::moveClip (*sides.ours, sides.clip, at (4.0), sides.tracks[1]);
        addMidiNote (*sides.theirs, sides.clip, 40, at (2.0), lasting (1.0));

        const auto merged = sides.merge();
        checkIDs (merged.song);
        CHECK (merged.clashes.empty());
        CHECK (findItems (merged.song, schema::CLIP, sides.clip.getRaw()).size() == 1);
        REQUIRE (clipOf (merged.song, "Drums").isValid());
        CHECK (countNotes (clipOf (merged.song, "Drums")) == 2);
        CHECK ((double) clipOf (merged.song, "Drums")[schema::clipStart] == doctest::Approx (4.0));
    }

    // Moved there onto a track it added, changed here.
    {
        Sides sides;
        const auto keys = phazite::insertTrack (*sides.theirs, sides.tracks[1], nullptr);
        phazite::setTrackName (*sides.theirs, keys, "Keys");
        phazite::moveClip (*sides.theirs, sides.clip, at (0.0), keys);
        addMidiNote (*sides.ours, sides.clip, 40, at (2.0), lasting (1.0));

        const auto merged = sides.merge();
        checkIDs (merged.song);
        CHECK (findItems (merged.song, schema::CLIP, sides.clip.getRaw()).size() == 1);
        REQUIRE (clipOf (merged.song, "Keys").isValid());
        CHECK (countNotes (clipOf (merged.song, "Keys")) == 2);
    }

    // Moved there off a track it then deleted, which this side left as it was: the track goes, the clip is where it went.
    {
        Sides sides;
        phazite::moveClip (*sides.theirs, sides.clip, at (0.0), sides.tracks[1]);
        phazite::deleteTracks (*sides.theirs, { sides.tracks[0] }, nullptr);

        const auto merged = sides.merge();
        checkIDs (merged.song);
        CHECK_FALSE (findTrack (merged.song, "Bass").isValid());
        CHECK (findItems (merged.song, schema::CLIP, sides.clip.getRaw()).size() == 1);
        CHECK (clipOf (merged.song, "Drums").isValid());
        CHECK (merged.notes.isEmpty());
    }

    // Moved there and nothing else changed: the merge takes both tracks' lines, never one alone, so it is on Drums, once
    // (the audit's case for a merge of some lines: the clip on neither track, or on both).
    {
        Sides sides;
        phazite::moveClip (*sides.theirs, sides.clip, at (4.0), sides.tracks[1]);
        const auto merged = sides.merge();
        checkIDs (merged.song);
        CHECK (findItems (merged.song, schema::CLIP, sides.clip.getRaw()).size() == 1);
        CHECK (clipOf (merged.song, "Drums").isValid());
        CHECK_FALSE (clipOf (merged.song, "Bass").isValid());
    }

    // Moved on both sides, to different tracks: where this side put it, with a note.
    {
        Sides sides;
        const auto keys = phazite::insertTrack (*sides.theirs, sides.tracks[1], nullptr);
        phazite::setTrackName (*sides.theirs, keys, "Keys");
        phazite::moveClip (*sides.theirs, sides.clip, at (0.0), keys);
        phazite::moveClip (*sides.ours, sides.clip, at (0.0), sides.tracks[1]);

        const auto merged = sides.merge();
        checkIDs (merged.song);
        CHECK (findItems (merged.song, schema::CLIP, sides.clip.getRaw()).size() == 1);
        CHECK (clipOf (merged.song, "Drums").isValid());
        CHECK (findTrack (merged.song, "Keys").isValid());
        CHECK (merged.notes.joinIntoString ("\n").contains ("moved on both sides: this side's place kept"));
    }
}

TEST_CASE ("A clip moved on one side and deleted on the other stays where it was moved, with a note, as a change and a removal do")
{
    const auto clipOf = [] (const juce::ValueTree& song, const juce::String& track)
    { return findTrack (song, track).getChildWithName (schema::CLIP); };

    // Moved there, deleted here: back where the other side moved it, and the note says so.
    {
        Sides sides;
        phazite::moveClip (*sides.theirs, sides.clip, at (4.0), sides.tracks[1]);
        phazite::deleteClips (*sides.ours, { sides.clip }, false);

        const auto merged = sides.merge();
        checkIDs (merged.song);
        CHECK (findItems (merged.song, schema::CLIP, sides.clip.getRaw()).size() == 1);
        CHECK (clipOf (merged.song, "Drums").isValid());
        INFO (merged.notes.joinIntoString ("\n").toStdString());
        CHECK (merged.notes.joinIntoString ("\n").contains ("Drums: clip \"Riff\" taken away here and moved there: kept"));
    }

    // Moved there onto a track it added, deleted here: on that track, with the note.
    {
        Sides sides;
        const auto keys = phazite::insertTrack (*sides.theirs, sides.tracks[1], nullptr);
        phazite::setTrackName (*sides.theirs, keys, "Keys");
        phazite::moveClip (*sides.theirs, sides.clip, at (0.0), keys);
        phazite::deleteClips (*sides.ours, { sides.clip }, false);

        const auto merged = sides.merge();
        checkIDs (merged.song);
        CHECK (findItems (merged.song, schema::CLIP, sides.clip.getRaw()).size() == 1);
        CHECK (clipOf (merged.song, "Keys").isValid());
        INFO (merged.notes.joinIntoString ("\n").toStdString());
        CHECK (merged.notes.joinIntoString ("\n").contains ("clip \"Riff\" taken away here and moved there: kept"));
    }

    // Moved here, deleted there: it stays where this side moved it, and the note says so.
    {
        Sides sides;
        phazite::moveClip (*sides.ours, sides.clip, at (4.0), sides.tracks[1]);
        phazite::deleteClips (*sides.theirs, { sides.clip }, false);

        const auto merged = sides.merge();
        checkIDs (merged.song);
        CHECK (findItems (merged.song, schema::CLIP, sides.clip.getRaw()).size() == 1);
        CHECK (clipOf (merged.song, "Drums").isValid());
        INFO (merged.notes.joinIntoString ("\n").toStdString());
        CHECK (merged.notes.joinIntoString ("\n").contains ("Bass: clip \"Riff\" moved here and taken away there: kept"));
    }

    // Moved on one side, left as it was on the other: no note.
    {
        Sides sides;
        phazite::moveClip (*sides.theirs, sides.clip, at (4.0), sides.tracks[1]);
        const auto merged = sides.merge();
        CHECK (clipOf (merged.song, "Drums").isValid());
        CHECK (merged.notes.isEmpty());
    }
}

TEST_CASE ("A device grouped into a container on one side and changed on the other is once in the song, in the container, as it changed")
{
    const auto makeSides = [] (Sides& sides)
    {
        const auto eq = phazite::insertDeviceState (*sides.base, sides.tracks[0],
                                                    phazite::makeOwnDeviceState (*sides.base, phazite::devices::Equalizer::typeID));
        sides.ours = phazite::makeSong (phazite::test::copyWholeSong (*sides.base));
        sides.theirs = phazite::makeSong (phazite::test::copyWholeSong (*sides.base));
        return eq;
    };

    const auto check = [] (const phazite::MergeResult& merged, phazite::DeviceID eq)
    {
        checkIDs (merged.song);
        CHECK (merged.clashes.empty());
        const auto devices = findItems (merged.song, schema::DEVICE, eq.getRaw());
        REQUIRE (devices.size() == 1);
        CHECK (devices.front().getParent().hasType (schema::CHAIN));
        CHECK ((float) devices.front().getChildWithName (schema::VALUES)["band4Gain"] == doctest::Approx (6.0f));
    };

    // Grouped here, changed there.
    {
        Sides sides;
        const auto eq = makeSides (sides);
        REQUIRE (phazite::groupIntoContainer (*sides.ours, { eq }).isValid());
        phazite::setValueSetByHand (*sides.theirs, { eq, "band4Gain" }, 6.0f);
        check (sides.merge(), eq);
    }

    // Grouped there, changed here.
    {
        Sides sides;
        const auto eq = makeSides (sides);
        REQUIRE (phazite::groupIntoContainer (*sides.theirs, { eq }).isValid());
        phazite::setValueSetByHand (*sides.ours, { eq, "band4Gain" }, 6.0f);
        check (sides.merge(), eq);
    }
}

namespace
{
// Sides whose base holds an alias group: the Bass's clip X (from beat 0, notes n at 0 and m at 2) and its alias Y on the
// Drums, from beat 8. Edits are made through either, as the piano roll makes them.
struct AliasSides : Sides
{
    AliasSides()
    {
        m = addMidiNote (*base, clip, 40, at (2.0), lasting (1.0)).note;
        n = phazite::getClipNotes (*base, clip).front().id;
        const auto aliases = phazite::copyClips (*base, { clip }, true);
        REQUIRE (aliases.size() == 1);
        alias = aliases.front();
        phazite::moveClip (*base, alias, at (8.0), tracks[1]);
        REQUIRE (phazite::findClipState (*base, alias)[schema::clipLink] == phazite::findClipState (*base, clip)[schema::clipLink]);
        ours = phazite::makeSong (phazite::test::copyWholeSong (*base));
        theirs = phazite::makeSong (phazite::test::copyWholeSong (*base));
    }

    // A note as a clip of the merged song holds it, applied to this side's (as Merge applies it).
    std::optional<phazite::NoteData> noteIn (phazite::ClipID in, phazite::NoteID note) const
    {
        return phazite::getNoteData (*ours, phazite::NoteRef { in, note });
    }

    // Whether X and Y of this side's song hold the same notes.
    bool holdTheSameNotes() const
    {
        return describeClipNotes (*ours, clip) == describeClipNotes (*ours, alias) && ! describeClipNotes (*ours, clip).isEmpty();
    }

    // This side's clips on both tracks, by their alias groups (none for a clip of its own).
    std::map<juce::String, std::vector<phazite::ClipID>> getGroups() const
    {
        std::map<juce::String, std::vector<phazite::ClipID>> groups;

        for (const auto track : tracks)
            for (const auto id : phazite::getClips (*ours, track))
                groups[phazite::findClipState (*ours, id)[schema::clipLink].toString()].push_back (id);

        return groups;
    }

    // Every alias group of this side's song with at least two clips, each holding the same notes; and the song holding together.
    void checkGroupsHoldTogether() const
    {
        for (const auto& [link, clips] : getGroups())
        {
            if (link.isEmpty())
                continue;

            INFO ("group " << link);
            CHECK (clips.size() >= 2);

            for (const auto id : clips)
                CHECK (describeClipNotes (*ours, id) == describeClipNotes (*ours, clips.front()));
        }

        checkSong (*ours);
    }

    // A third clip of the group, W, on the Drums from beat 16, in the base and so on both sides.
    phazite::ClipID addThirdToBase()
    {
        const auto copies = phazite::copyClips (*base, { clip }, true);
        REQUIRE (copies.size() == 1);
        phazite::moveClip (*base, copies.front(), at (16.0), tracks[1]);
        ours = phazite::makeSong (phazite::test::copyWholeSong (*base));
        theirs = phazite::makeSong (phazite::test::copyWholeSong (*base));
        return copies.front();
    }

    phazite::ClipID alias;
    phazite::NoteID n, m;
};
} // namespace

TEST_CASE ("Merged edits of the notes of a clip that has an alias reach both, and the merged song holds together")
{
    SUBCASE ("Different notes: one moved here through the clip, another's velocity set there through its alias")
    {
        AliasSides sides;
        phazite::moveNote (*sides.ours, { sides.clip, sides.n }, at (1.0), 38);
        phazite::setNoteVelocities (*sides.theirs, { { sides.alias, sides.m } }, 50);

        const auto merged = sides.mergeIntoOurs();
        CHECK (merged.clashes.empty());
        CHECK (sides.holdTheSameNotes());
        CHECK (sides.noteIn (sides.alias, sides.n).value().pitch == 38);
        CHECK (sides.noteIn (sides.clip, sides.m).value().velocity == 50);
        checkSong (*sides.ours);
    }

    SUBCASE ("The same note's different values: its pitch here, its velocity there")
    {
        AliasSides sides;
        phazite::transposeNotes (*sides.ours, { { sides.clip, sides.n } }, 2);
        phazite::setNoteVelocities (*sides.theirs, { { sides.alias, sides.n } }, 64);

        const auto merged = sides.mergeIntoOurs();
        CHECK (merged.clashes.empty());
        CHECK (sides.holdTheSameNotes());
        CHECK (sides.noteIn (sides.alias, sides.n).value().pitch == 38);
        CHECK (sides.noteIn (sides.alias, sides.n).value().velocity == 64);
        checkSong (*sides.ours);
    }

    SUBCASE ("The same value changed differently on both sides: a clash, reported once, this side's value kept in both clips")
    {
        AliasSides sides;
        phazite::transposeNotes (*sides.ours, { { sides.clip, sides.n } }, 2);
        phazite::transposeNotes (*sides.theirs, { { sides.alias, sides.n } }, 5);

        const auto merged = sides.mergeIntoOurs();
        REQUIRE (merged.clashes.size() == 1); // for the group's keeper, X, alone
        CHECK (merged.clashes[0].property == schema::notePitch);
        CHECK (merged.clashes[0].path.contains ("CLIP:" + juce::String ((juce::int64) sides.clip.getRaw())));
        CHECK ((int) merged.clashes[0].theirs == 41);
        CHECK (sides.holdTheSameNotes());
        CHECK (sides.noteIn (sides.clip, sides.n).value().pitch == 38);
        CHECK (sides.noteIn (sides.alias, sides.n).value().pitch == 38);
        checkSong (*sides.ours);
    }

    SUBCASE ("A note added here through the alias, another deleted there through the clip")
    {
        AliasSides sides;
        const auto added = phazite::addNote (*sides.ours, sides.alias, 43, at (12.0), lasting (1.0), 90);
        REQUIRE (added.isValid());
        phazite::deleteNotes (*sides.theirs, { { sides.clip, sides.m } });

        const auto merged = sides.mergeIntoOurs();
        CHECK (merged.clashes.empty());
        CHECK (sides.holdTheSameNotes());
        CHECK (sides.noteIn (sides.clip, added.note).has_value());
        CHECK_FALSE (sides.noteIn (sides.alias, sides.m).has_value());
        CHECK (phazite::getClipNotes (*sides.ours, sides.alias).size() == 2);
        checkSong (*sides.ours);
    }

    SUBCASE ("The alias made unique on one side, a note's velocity set through the clip on the other: both clips their own, "
             "each with the note as set")
    {
        for (const auto uniqueHere : { true, false })
        {
            INFO ((uniqueHere ? "made unique here" : "made unique there"));
            AliasSides sides;
            phazite::makeClipsUnique (uniqueHere ? *sides.ours : *sides.theirs, { sides.alias });
            phazite::setNoteVelocities (uniqueHere ? *sides.theirs : *sides.ours, { { sides.clip, sides.n } }, 70);

            const auto merged = sides.mergeIntoOurs();
            CHECK (merged.clashes.empty());
            CHECK_FALSE (phazite::findClipState (*sides.ours, sides.clip).hasProperty (schema::clipLink));
            CHECK_FALSE (phazite::findClipState (*sides.ours, sides.alias).hasProperty (schema::clipLink));
            CHECK (sides.noteIn (sides.clip, sides.n).value().velocity == 70);
            CHECK (sides.noteIn (sides.alias, sides.n).value().velocity == 70); // the change made to it, while it was an alias
            checkSong (*sides.ours);
        }
    }
}

TEST_CASE ("An alias added on one side of a clip whose notes the other side changed holds the clip's notes as merged")
{
    // Schema.h, clipLink: "The clips of a group hold the same notes, with the same IDs". One side adds an alias Z of X on the
    // Bass, from beat 16; the other moves n, through X or through Y.
    for (const auto addedHere : { false, true })
    {
        for (const auto throughAlias : { false, true })
        {
            INFO ((addedHere ? "added here" : "added there") << (throughAlias ? ", moved through Y" : ", moved through X"));
            AliasSides sides;
            auto& adding = addedHere ? *sides.ours : *sides.theirs;
            auto& moving = addedHere ? *sides.theirs : *sides.ours;
            const auto copies = phazite::copyClips (adding, { sides.clip }, true);
            REQUIRE (copies.size() == 1);
            phazite::moveClip (adding, copies.front(), at (16.0), sides.tracks[0]);
            phazite::moveNote (moving, { throughAlias ? sides.alias : sides.clip, sides.n }, at (throughAlias ? 9.0 : 1.0), 38);

            const auto merged = sides.mergeIntoOurs();
            CHECK (merged.clashes.empty());
            const auto groups = sides.getGroups();
            REQUIRE (groups.size() == 1); // X, Y and Z in one group
            REQUIRE (groups.begin()->second.size() == 3);

            for (const auto id : groups.begin()->second)
                CHECK (sides.noteIn (id, sides.n).value().pitch == 38);

            sides.checkGroupsHoldTogether();
        }
    }
}

TEST_CASE ("An alias deleted on one side and changed on the other comes back into a group that holds together")
{
    // Deleting Y unlinks X, the last of its group (Clips.h); Y changed there comes back (taken away here, changed there: it
    // stays), alone in its group, which it leaves: a clip of its own, with its notes as changed.
    for (const auto deletedHere : { true, false })
    {
        INFO ((deletedHere ? "deleted here" : "deleted there"));
        AliasSides sides;
        phazite::deleteClips (deletedHere ? *sides.ours : *sides.theirs, { sides.alias }, false);
        REQUIRE_FALSE (phazite::findClipState (deletedHere ? *sides.ours : *sides.theirs, sides.clip).hasProperty (schema::clipLink));
        phazite::setNoteVelocities (deletedHere ? *sides.theirs : *sides.ours, { { sides.alias, sides.n } }, 70);

        const auto merged = sides.mergeIntoOurs();
        CHECK (merged.clashes.empty());
        REQUIRE (phazite::findClipState (*sides.ours, sides.alias).isValid()); // kept: changed on one side
        CHECK_FALSE (phazite::findClipState (*sides.ours, sides.alias).hasProperty (schema::clipLink));
        CHECK_FALSE (phazite::findClipState (*sides.ours, sides.clip).hasProperty (schema::clipLink));
        CHECK (sides.noteIn (sides.alias, sides.n).value().velocity == 70);
        CHECK (sides.noteIn (sides.clip, sides.n).value().velocity == 70); // changed while it was Y's alias
        sides.checkGroupsHoldTogether();
    }
}

TEST_CASE ("An alias added on one side and the original clip deleted on the other leave clips that hold together")
{
    // One side adds Z, an alias of X on the Bass from beat 16, and sets n's velocity through it (X and Y take it); the other
    // deletes X (Y, the last of its group, loses its link). X comes back, changed there; X and Z make the group, Y holds the
    // same notes as a clip of its own.
    for (const auto addedHere : { true, false })
    {
        INFO ((addedHere ? "added here" : "added there"));
        AliasSides sides;
        auto& adding = addedHere ? *sides.ours : *sides.theirs;
        auto& deleting = addedHere ? *sides.theirs : *sides.ours;
        const auto copies = phazite::copyClips (adding, { sides.clip }, true);
        REQUIRE (copies.size() == 1);
        phazite::moveClip (adding, copies.front(), at (16.0), sides.tracks[0]);
        phazite::setNoteVelocities (adding, { { copies.front(), sides.n } }, 70);
        phazite::deleteClips (deleting, { sides.clip }, false);

        const auto merged = sides.mergeIntoOurs();
        CHECK (merged.clashes.empty());
        CHECK_FALSE (phazite::findClipState (*sides.ours, sides.alias).hasProperty (schema::clipLink));
        CHECK (sides.noteIn (sides.alias, sides.n).value().velocity == 70);

        for (const auto& [link, clips] : sides.getGroups())
            for (const auto id : clips)
                CHECK (describeClipNotes (*sides.ours, id) == describeClipNotes (*sides.ours, sides.alias));

        sides.checkGroupsHoldTogether();
    }
}

TEST_CASE ("A clip made unique and changed on one side while the other changes its group: the clip its own with both changes, "
           "the group holding together")
{
    // X, Y and W in one group. One side makes Y unique and sets n's velocity in it; the other transposes m through W, which X,
    // Y and W all took there.
    for (const auto uniqueHere : { true, false })
    {
        INFO ((uniqueHere ? "made unique here" : "made unique there"));
        AliasSides sides;
        const auto third = sides.addThirdToBase();
        auto& unique = uniqueHere ? *sides.ours : *sides.theirs;
        auto& group = uniqueHere ? *sides.theirs : *sides.ours;
        phazite::makeClipsUnique (unique, { sides.alias });
        REQUIRE_FALSE (phazite::findClipState (unique, sides.alias).hasProperty (schema::clipLink));
        REQUIRE (phazite::findClipState (unique, sides.clip).hasProperty (schema::clipLink)); // X and W still a group
        phazite::setNoteVelocities (unique, { { sides.alias, sides.n } }, 30);
        phazite::transposeNotes (group, { { third, sides.m } }, 3);

        const auto merged = sides.mergeIntoOurs();
        CHECK (merged.clashes.empty());
        CHECK_FALSE (phazite::findClipState (*sides.ours, sides.alias).hasProperty (schema::clipLink));
        CHECK (phazite::findClipState (*sides.ours, sides.clip).hasProperty (schema::clipLink));
        CHECK (describeClipNotes (*sides.ours, sides.clip) == describeClipNotes (*sides.ours, third));
        CHECK (sides.noteIn (sides.clip, sides.m).value().pitch == 43);
        CHECK (sides.noteIn (sides.clip, sides.n).value().velocity == 100);
        CHECK (sides.noteIn (sides.alias, sides.m).value().pitch == 43); // the change made to it, while it was an alias
        CHECK (sides.noteIn (sides.alias, sides.n).value().velocity == 30);
        sides.checkGroupsHoldTogether();
    }
}

TEST_CASE ("A group both sides added a clip to holds together, each side's changes made through its own new clip in every one")
{
    // This side adds Z1, an alias of X on the Bass from beat 16, and moves m through it; the other adds Z2, an alias of Y on the
    // Drums from beat 24 (under the same new ID: both count on from the same place, so the merge gives it another), and sets
    // n's velocity through it.
    AliasSides sides;
    const auto ourCopies = phazite::copyClips (*sides.ours, { sides.clip }, true);
    const auto theirCopies = phazite::copyClips (*sides.theirs, { sides.alias }, true);
    REQUIRE (ourCopies.size() == 1);
    REQUIRE (theirCopies.size() == 1);
    REQUIRE (ourCopies.front() == theirCopies.front());
    phazite::moveClip (*sides.ours, ourCopies.front(), at (16.0), sides.tracks[0]);
    phazite::moveClip (*sides.theirs, theirCopies.front(), at (24.0), sides.tracks[1]);
    phazite::moveNote (*sides.ours, { ourCopies.front(), sides.m }, at (20.0), 45);
    phazite::setNoteVelocities (*sides.theirs, { { theirCopies.front(), sides.n } }, 55);

    const auto merged = sides.mergeIntoOurs();
    CHECK (merged.clashes.empty());
    const auto groups = sides.getGroups();
    REQUIRE (groups.size() == 1);
    REQUIRE (groups.begin()->second.size() == 4); // X, Y, Z1 and Z2

    for (const auto id : groups.begin()->second)
    {
        CHECK (sides.noteIn (id, sides.m).value().pitch == 45);
        CHECK (sides.noteIn (id, sides.n).value().velocity == 55);
    }

    sides.checkGroupsHoldTogether();
}

TEST_CASE ("Notes both sides added to a group under one ID: the other side's given a new ID, the same in every clip of the "
           "group, as its record says")
{
    // Both sides add a note through a different clip of the group; both count on from the same place, so the notes share an
    // ID, and the merge gives the other side's a new one, recorded by the group.
    AliasSides sides;
    const auto ours = phazite::addNote (*sides.ours, sides.clip, 50, at (5.0), lasting (1.0), 90);
    const auto theirs = phazite::addNote (*sides.theirs, sides.alias, 52, at (13.0), lasting (1.0), 80);
    REQUIRE (ours.note == theirs.note);

    const auto merged = sides.mergeIntoOurs();
    CHECK (merged.clashes.empty());
    const auto link = phazite::findClipState (*sides.ours, sides.clip)[schema::clipLink].toString();
    std::optional<phazite::RenumberedID> given;

    for (const auto& id : merged.renumbered)
        if (id.clip.isNotEmpty())
            given = id;

    REQUIRE (given.has_value());
    CHECK (given->clip == "link " + link);
    CHECK (given->from == theirs.note.getRaw());

    for (const auto id : { sides.clip, sides.alias })
    {
        CHECK (sides.noteIn (id, ours.note).value().pitch == 50);
        CHECK (sides.noteIn (id, phazite::NoteID::fromRaw (given->to)).value().pitch == 52);
    }

    sides.checkGroupsHoldTogether();
}

namespace
{
// What a merge reported, for a failure's message: each clash's path and property, and each of its notes.
juce::String describeMerge (const phazite::MergeResult& merged)
{
    juce::StringArray lines;

    for (const auto& clash : merged.clashes)
        lines.add ("clash " + clash.path.joinIntoString ("/") + " " + clash.property.toString());

    lines.addArray (merged.notes);
    return lines.joinIntoString ("; ");
}
} // namespace

TEST_CASE ("A clip copied as an alias on one side and its notes changed on the other: the alias and the clip both hold the change")
{
    // X has no link where the lines part. One side copies it as an alias Z, on the Bass from beat 16 (X and Z now a group,
    // which the parting didn't hold: the group's keeper is X, the first clip both sides hold); the other moves X's note and
    // sets its velocity.
    for (const auto aliasedHere : { true, false })
    {
        INFO ((aliasedHere ? "aliased here, changed there" : "aliased there, changed here"));
        Sides sides;
        auto& aliasing = aliasedHere ? *sides.ours : *sides.theirs;
        auto& changing = aliasedHere ? *sides.theirs : *sides.ours;
        const auto note = phazite::getClipNotes (*sides.base, sides.clip).front().id;
        const auto copies = phazite::copyClips (aliasing, { sides.clip }, true);
        REQUIRE (copies.size() == 1);
        const auto z = copies.front();
        phazite::moveClip (aliasing, z, at (16.0), sides.tracks[0]);
        phazite::moveNote (changing, { sides.clip, note }, at (1.0), 38);
        phazite::setNoteVelocities (changing, { { sides.clip, note } }, 45);

        const auto merged = sides.mergeIntoOurs();
        INFO (describeMerge (merged));
        CHECK (merged.clashes.empty());
        const auto link = phazite::findClipState (*sides.ours, sides.clip)[schema::clipLink].toString();
        CHECK (link.isNotEmpty());
        CHECK (phazite::findClipState (*sides.ours, z)[schema::clipLink].toString() == link);

        for (const auto id : { sides.clip, z })
        {
            INFO ("clip " << id.getRaw());
            const auto held = phazite::getNoteData (*sides.ours, phazite::NoteRef { id, note });
            REQUIRE (held.has_value());
            CHECK (held->pitch == 38);
            CHECK (held->velocity == 45);
        }

        CHECK (describeClipNotes (*sides.ours, sides.clip) == describeClipNotes (*sides.ours, z));
        checkSong (*sides.ours);
    }
}

TEST_CASE ("Every clip of a group deleted on one side and the group changed through one of them on the other: the group stays, "
           "changed, and holds together")
{
    // Taken away on one side, changed on the other: it stays (X changed with Y, its alias). No clip of the group is one both
    // sides hold, so the keeper is the group's first clip in the merged song's order, X.
    for (const auto deletedHere : { true, false })
    {
        INFO ((deletedHere ? "deleted here, changed there" : "deleted there, changed here"));
        AliasSides sides;
        phazite::deleteClips (deletedHere ? *sides.ours : *sides.theirs, { sides.clip, sides.alias }, false);
        phazite::setNoteVelocities (deletedHere ? *sides.theirs : *sides.ours, { { sides.alias, sides.n } }, 70);

        const auto merged = sides.mergeIntoOurs();
        INFO (describeMerge (merged));
        CHECK (merged.clashes.empty());
        REQUIRE (phazite::findClipState (*sides.ours, sides.clip).isValid());
        REQUIRE (phazite::findClipState (*sides.ours, sides.alias).isValid());
        const auto link = phazite::findClipState (*sides.ours, sides.clip)[schema::clipLink].toString();
        CHECK (link.isNotEmpty());
        CHECK (phazite::findClipState (*sides.ours, sides.alias)[schema::clipLink].toString() == link);
        CHECK (sides.holdTheSameNotes());
        CHECK (sides.noteIn (sides.clip, sides.n).value().velocity == 70);
        CHECK (sides.noteIn (sides.alias, sides.n).value().velocity == 70);
        sides.checkGroupsHoldTogether();
    }
}

TEST_CASE ("A clip both sides copied as the same alias: the copy is the group's keeper, and the group holds together")
{
    // X has no link where the lines part; both sides copy it as an alias to the same place, Z, under the same IDs (both count
    // on from the same place): Z is the same on both sides and keeps its IDs, X's new link on the other side is given anew.
    // The keeper is the first clip both sides hold in the merged group, Z (the parting held none of it).
    Sides sides;
    std::vector<phazite::ClipID> copies;

    for (auto* side : { sides.ours.get(), sides.theirs.get() })
    {
        const auto copied = phazite::copyClips (*side, { sides.clip }, true);
        REQUIRE (copied.size() == 1);
        phazite::moveClip (*side, copied.front(), at (16.0), sides.tracks[0]);
        copies.push_back (copied.front());
    }

    REQUIRE (copies[0] == copies[1]);
    phazite::setTrackName (*sides.theirs, sides.tracks[1], "Beat"); // a change outside the group

    const auto merged = sides.mergeIntoOurs();
    INFO (describeMerge (merged));

    // X's link, new on both sides, is a clash (the other side's given anew), this side's kept: X is in Z's group.
    REQUIRE (merged.clashes.size() == 1);
    CHECK (merged.clashes[0].property == schema::clipLink);
    CHECK (merged.clashes[0].path[merged.clashes[0].path.size() - 1] == "CLIP:" + juce::String ((juce::int64) sides.clip.getRaw()));
    const auto link = phazite::findClipState (*sides.ours, copies[0])[schema::clipLink].toString();
    CHECK (link.isNotEmpty());
    CHECK (phazite::findClipState (*sides.ours, sides.clip)[schema::clipLink].toString() == link);
    CHECK (describeClipNotes (*sides.ours, sides.clip) == describeClipNotes (*sides.ours, copies[0]));
    checkSong (*sides.ours);
}
