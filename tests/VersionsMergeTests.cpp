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

    // The other side's changes, every line of them (a merge takes them all), merged into this side's.
    phazite::MergeResult merge()
    {
        return phazite::mergeSongs (base->getState(), ours->getState(), theirs->getState(), [this] { return ours->makeID(); });
    }

    // The merge applied to this side's song, as Merge applies it (VersionsModel.cpp): one step, what names the gone taken away.
    phazite::MergeResult mergeIntoOurs()
    {
        auto merged = merge();
        const phazite::UndoStep step (*ours, "Merge");
        phazite::makeSongLike (*ours, merged.song);
        phazite::removeBrokenReferences (*ours);
        return merged;
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
    sides.ours = phazite::makeSong (sides.base->getState().createCopy());
    sides.theirs = phazite::makeSong (sides.base->getState().createCopy());
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
    sides.ours = phazite::makeSong (sides.base->getState().createCopy());
    sides.theirs = phazite::makeSong (sides.base->getState().createCopy());

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
    const auto parted = sides.theirs->getState().createCopy();
    findTrack (sides.theirs->getState(), "Pad").setProperty (schema::trackName, "Pad 2", nullptr);

    // Merged again, the parting's and the other side's songs numbered as this side's through what the first merge recorded.
    auto base = parted.createCopy(), theirs = sides.theirs->getState().createCopy();
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
    const auto base = song->getState().createCopy();
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
    const auto theirs = song->getState().createCopy();
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
    const auto theirs = song->getState().createCopy();
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
        auto copy = song->getState().createCopy();

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
    const auto base = song->getState().createCopy();
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
    auto acrossClips = song->getState().createCopy();
    juce::ValueTree (notesOf (acrossClips, second)[0])
        .setProperty (schema::noteID, notesOf (acrossClips, first)[0][schema::noteID], nullptr);
    CHECK (phazite::holdsEachIDOnce (acrossClips));

    // Two notes of one clip, or two tracks, under one ID: twice.
    auto inOneClip = song->getState().createCopy();
    const auto notes = notesOf (inOneClip, first);
    REQUIRE (notes.size() == 2);
    juce::ValueTree (notes[1]).setProperty (schema::noteID, notes[0][schema::noteID], nullptr);
    CHECK_FALSE (phazite::holdsEachIDOnce (inOneClip));

    auto twoTracks = song->getState().createCopy();
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
        sides.ours = phazite::makeSong (sides.base->getState().createCopy());
        sides.theirs = phazite::makeSong (sides.base->getState().createCopy());
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
