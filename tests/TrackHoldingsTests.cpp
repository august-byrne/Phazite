// SPDX-License-Identifier: AGPL-3.0-or-later
// What takes a track out of the song erases what it holds in the song's areas beside its tree first, in its own step
// (model/arrangement/internal/Tracks.h: eraseTrackHoldings(); roadmap step S5.20, the tracks' keepers' end): its clips with
// their notes, its channel's modulators and macros and its containers' macros, its sends; and each area's own undo action
// brings its things back with the track's.
// Each remover: Delete (a track with a return, a folder with its tracks), Ungroup (the folder alone), a version's line of a
// track added since, a version applied whole (a track gone, a track moved), and a merge's choice of a whole track or the
// master, which gives the side's track its things through each area's code, leaving no adoption to the keeper.
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UndoCheck.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using phazite::ClipID;
using phazite::DeviceID;
using phazite::ModulatorID;
using phazite::ParameterRef;
using phazite::TrackID;
namespace schema = phazite::schema;

namespace
{
// A song whose tracks hold something of every area beside the tree, and devices:
//   - stays: a MIDI track that stays, holding an alias of track's clip (the group's notes stay with it), and a send to the
//     return that goes;
//   - keptReturn: a return that stays;
//   - track: a synth with an EQ off its default, the tests' plug-in with its own state, a container whose first macro is set
//     and automated, an LFO modulating its pan, a MIDI clip with notes in an alias group with stays' alias, a send to
//     keptReturn;
//   - goneReturn: a return with a modulator and a send to keptReturn (a send from a return that goes);
//   - group: a bus with a modulator, holding audio (an audio clip, a send to keptReturn) and sub, a bus with a modulator,
//     holding inner (a MIDI clip with a note).
struct Holdings
{
    phazite::test::TestPlugin plugin; // before the song that holds one
    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    TrackID stays, keptReturn, track, goneReturn, group, sub, audio, inner;
    ClipID clip, alias, audioClip, innerClip;
    DeviceID eq, effect, container;
    ModulatorID lfo, macro, returnLfo, groupLfo, subLfo;
    ParameterRef macroValue;

    Holdings()
    {
        stays = s.firstTrack;
        s.rename (stays, "Stays");
        track = s.addTrack ("Track");
        s.setInstrument (track, phazite::devices::Synth::typeID);
        eq = s.addEffect (track, phazite::devices::Equalizer::typeID);
        const auto outputID = phazite::devices::Equalizer::info.parameters[(size_t) phazite::devices::Equalizer::outputParameter].id;
        s.setParameter (eq, juce::String (outputID.data(), outputID.size()), -3.0f);

        {
            const phazite::UndoStep step (song, "Add Plug-in");
            effect = phazite::test::TestPlugin::add (song, track);
        }

        REQUIRE (effect.isValid());
        s.waitForPlugins();
        phazite::test::TestPlugin::setGain (song, effect, 0.5f);
        song.storeDeviceStates();

        container = phazite::addContainer (s.context, track);
        REQUIRE (container.isValid());
        const auto macros = phazite::getContainerMacros (song, container);
        REQUIRE_FALSE (macros.empty());
        macro = macros[0];
        s.setMacroValue (macro, 0.25f);
        macroValue = { DeviceID::fromRaw (macro.getRaw()), juce::String (macro.getRaw()) }; // a macro's one parameter
        s.addAutomation (macroValue, { { phazite::test::at (0.0), 0.2f }, { phazite::test::at (4.0), 0.8f } });
        lfo = s.addModulator (track, phazite::ModulatorType::lfo);
        s.modulate (lfo, s.getPanParameter (track), 0.5f);

        clip = s.addMidiClip (track, 0.0, 4.0);
        s.addNote (clip, 60, 0.0, 1.0);
        s.addNote (clip, 64, 1.0, 1.0);
        alias = s.copyClip (clip, 8.0, true);

        {
            const phazite::UndoStep step (song, "Move Clip");
            phazite::moveClip (song, alias, phazite::test::at (8.0), stays);
        }

        REQUIRE (phazite::getClipTrack (song, alias) == stays);
        keptReturn = s.addReturn (track, "phaziteReverb", -6.0f);
        goneReturn = s.addReturn (stays, "phaziteDelay", -12.0f);
        returnLfo = s.addModulator (goneReturn, phazite::ModulatorType::lfo);

        {
            const phazite::UndoStep step (song, "Add Send");
            REQUIRE (phazite::addSend (song, goneReturn, keptReturn, false).isValid());
        }

        inner = s.addTrack ("Inner");
        innerClip = s.addMidiClip (inner, 0.0, 4.0);
        s.addNote (innerClip, 67, 0.0, 2.0);
        sub = s.addBus ("Sub", { inner });
        subLfo = s.addModulator (sub, phazite::ModulatorType::lfo);
        audio = s.addTrack ("Audio");
        audioClip = s.addAudioClip (audio, phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Loop.wav"), 0.5), 0.0);
        s.addSend (audio, keptReturn, -9.0f);
        group = s.addBus ("Group", { audio, sub });
        groupLfo = s.addModulator (group, phazite::ModulatorType::lfo);

        REQUIRE (phazite::getParentFolder (song, audio) == group);
        REQUIRE (phazite::getParentFolder (song, sub) == group);
        REQUIRE (phazite::getParentFolder (song, inner) == sub);
        REQUIRE (phazite::getSends (song, track).size() == 1);
        REQUIRE (phazite::getSends (song, goneReturn).size() == 1);
        REQUIRE (phazite::getSends (song, audio).size() == 1);
        phazite::test::checkSong (song);
        song.getUndoManager().clearUndoHistory();
    }

    const phazite::SongValues& values() const { return *song.getValues(); }

    // Nothing of these tracks in any area: no clip on one, no modulator or macro placed on one, no send from one.
    void checkNothingOf (const std::vector<TrackID>& gone) const
    {
        for (const auto id : gone)
        {
            INFO ("Track " << (juce::int64) id.getRaw());
            CHECK (values().clips.get().tracks.find (id.getRaw()) == nullptr);
            CHECK (values().sends.get().tracks.find (id.getRaw()) == nullptr);

            for (const auto& [key, value] : values().clips.get().clips)
                CHECK (value.track != id);

            for (const auto& [key, value] : values().modulation.get().modulators)
                CHECK (value.place.track != id);

            for (const auto& [key, value] : values().sends.get().sends)
                CHECK (value.from != id);
        }

        phazite::test::checkSong (song);
    }

    // A remover, as one named undo step (an UndoCheck around it), then Undo, Redo and Undo again: nothing of what goes left
    // in any area after it, and the song whole as it was after each Undo.
    void checkRemoval (const juce::String& name, const std::function<void()>& remove, const std::vector<TrackID>& gone)
    {
        const auto before = phazite::test::writeSong (song, phazite::test::SongPart::whole);
        phazite::test::UndoCheck undo (s.context);

        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (song, name);
                        remove();
                    });

        CHECK (song.getUndoManager().getUndoDescription() == name);
        checkNothingOf (gone);

        const auto after = phazite::test::writeSong (song, phazite::test::SongPart::whole);

        for (const auto* step : { "Undo", "Redo", "Undo again" })
        {
            const auto isUndo = juce::String (step).startsWith ("Undo");
            undo.stepBackOrForth (step, [this, isUndo] { isUndo ? phazite::undo (song) : phazite::redo (song); });
            INFO (step);
            phazite::test::checkSong (song);

            const auto now = phazite::test::writeSong (song, phazite::test::SongPart::whole);
            const auto& expected = isUndo ? before : after;
            INFO (phazite::test::describeSongDifferences (expected, now).joinIntoString ("\n"));
            CHECK (now == expected);

            if (! isUndo)
                checkNothingOf (gone);
        }
    }

    // The song whole after a change made and undone: a version of it, as the versions hold one.
    juce::ValueTree versionAfter (const std::function<void()>& change)
    {
        {
            const phazite::UndoStep step (song, "For the Version");
            change();
        }

        const auto version = phazite::test::copyWholeSong (song);
        phazite::undo (song);
        song.getUndoManager().clearUndoHistory();
        return version;
    }
};

// An element's place from the song down, as a merge's clash names it ("TRACKS:TRACKS", "TRACK:12").
juce::StringArray pathOf (const juce::ValueTree& element)
{
    juce::StringArray path;

    for (auto each = element; each.isValid() && each.getParent().isValid(); each = each.getParent())
        path.insert (0, each.getType().toString() + ":" + schema::getElementKey (each));

    return path;
}
} // namespace

TEST_CASE ("Track holdings: deleting a track and a return erases what they hold, and the alias group's notes stay with the clip "
           "on the track that stays")
{
    Holdings h;
    const auto aliasNotes = phazite::getClipNotesValue (h.song, h.alias);
    REQUIRE (aliasNotes.notes.size() == 2);

    h.checkRemoval ("Delete Tracks", [&h] { phazite::deleteTracks (h.song, { h.track, h.goneReturn }, nullptr); },
                    { h.track, h.goneReturn });

    // Redone: the alias left alone in its group keeps its notes (unlinked as the last of its group); what named what went
    // (stays' send to the return gone) went with it, and so did the curve of its container's macro.
    phazite::redo (h.song);
    REQUIRE (phazite::getClipValue (h.song, h.alias).has_value());
    CHECK (phazite::getClipNotesValue (h.song, h.alias) == aliasNotes);
    CHECK (phazite::getClipValue (h.song, h.alias)->link == 0);
    CHECK (phazite::getSends (h.song, h.stays).empty());
    CHECK (phazite::findCurve (h.song, h.macroValue) == nullptr);
    CHECK_FALSE (phazite::getModulatorValue (h.song, h.lfo).has_value());
    CHECK_FALSE (phazite::getModulatorValue (h.song, h.macro).has_value());
    CHECK_FALSE (phazite::getModulatorValue (h.song, h.returnLfo).has_value());
}

TEST_CASE ("Track holdings: deleting a folder erases what it and the tracks in it hold, a bus inside it too")
{
    Holdings h;
    h.checkRemoval ("Delete Track", [&h] { phazite::deleteTracks (h.song, { h.group }, nullptr); }, { h.group, h.sub, h.audio, h.inner });

    phazite::redo (h.song);
    CHECK_FALSE (phazite::getClipValue (h.song, h.audioClip).has_value());
    CHECK_FALSE (phazite::getClipValue (h.song, h.innerClip).has_value());
    CHECK_FALSE (phazite::getModulatorValue (h.song, h.groupLfo).has_value());
    CHECK_FALSE (phazite::getModulatorValue (h.song, h.subLfo).has_value());
}

TEST_CASE ("Track holdings: ungrouping a folder erases what the folder holds alone, and its tracks keep theirs")
{
    Holdings h;
    const auto audioSends = phazite::getSends (h.song, h.audio);
    h.checkRemoval ("Ungroup Folder", [&h] { phazite::ungroupFolder (h.song, h.group); }, { h.group });

    phazite::redo (h.song);
    CHECK_FALSE (phazite::getParentFolder (h.song, h.audio).isValid());
    CHECK (phazite::getClips (h.song, h.audio) == std::vector { h.audioClip });
    CHECK (phazite::getSends (h.song, h.audio) == audioSends);
    CHECK (phazite::getClips (h.song, h.inner) == std::vector { h.innerClip });
    CHECK (phazite::getModulators (h.song, h.sub) == std::vector { h.subLfo });
    CHECK_FALSE (phazite::getModulatorValue (h.song, h.groupLfo).has_value());
}

TEST_CASE ("Track holdings: a version's line of a track added since takes it away with what it holds, its folder's tracks staying")
{
    Holdings h;

    SUBCASE ("A track")
    {
        const auto version = h.versionAfter ([&h] { phazite::deleteTracks (h.song, { h.track }, nullptr); });
        h.checkRemoval ("Restore Track",
                        [&h, &version]
                        {
                            REQUIRE (phazite::makeTrackLike (h.song, h.track, version));
                            phazite::removeBrokenReferences (h.song);
                        },
                        { h.track });
    }

    SUBCASE ("A folder, whose tracks stay where it was")
    {
        const auto version = h.versionAfter ([&h] { phazite::ungroupFolder (h.song, h.group); });
        h.checkRemoval ("Restore Track",
                        [&h, &version]
                        {
                            REQUIRE (phazite::makeTrackLike (h.song, h.group, version));
                            phazite::removeBrokenReferences (h.song);
                        },
                        { h.group });

        phazite::redo (h.song);
        CHECK (phazite::getClips (h.song, h.audio) == std::vector { h.audioClip });
        CHECK (phazite::getModulators (h.song, h.sub) == std::vector { h.subLfo });
    }
}

namespace
{
// A song for what a remover leaves of an alias group: Older's clip with two notes, which an alias on another track joins.
struct AliasAcrossTracks
{
    AliasAcrossTracks()
    {
        clip = s.addMidiClip (older, 0.0, 4.0);
        s.addNote (clip, 60, 0.0, 1.0);
        s.addNote (clip, 64, 1.0, 1.0);
        notes = phazite::getClipNotesValue (song, clip);
        REQUIRE (notes.notes.size() == 2);
    }

    // An alias of Older's clip made on another track.
    void makeAlias (TrackID onto)
    {
        alias = s.copyClip (clip, 8.0, true);

        {
            const phazite::UndoStep step (song, "Move Clip");
            phazite::moveClip (song, alias, phazite::test::at (8.0), onto);
        }

        REQUIRE (phazite::getClipTrack (song, alias) == onto);
        REQUIRE (phazite::getAliasClips (song, clip) == std::vector { clip, alias });
        song.getUndoManager().clearUndoHistory();
    }

    // A remover, as one named undo step, then Undo and Redo: after it, the clip that stays is no alias any more and holds the
    // group's notes, and the song passes its check whole; after the Undo, the group of two is back.
    void checkLeftNoAlias (const juce::String& name, const std::function<void()>& remove, ClipID stays)
    {
        phazite::test::UndoCheck undo (s.context);

        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (song, name);
                        remove();
                    });

        const auto checkUnlinked = [this, stays]
        {
            REQUIRE (phazite::getClipValue (song, stays).has_value());
            CHECK (phazite::getClipValue (song, stays)->link == 0);
            CHECK (phazite::getAliasClips (song, stays) == std::vector { stays });
            CHECK (phazite::getClipNotesValue (song, stays) == notes);
            phazite::test::checkSong (song);
        };

        checkUnlinked();
        undo.stepBackOrForth ("Undo", [this] { phazite::undo (song); });
        CHECK (phazite::getAliasClips (song, clip) == std::vector { clip, alias });
        CHECK (phazite::getClipNotesValue (song, alias) == notes);
        phazite::test::checkSong (song);
        undo.stepBackOrForth ("Redo", [this] { phazite::redo (song); });
        checkUnlinked();
    }

    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    TrackID older = s.firstTrack;
    ClipID clip, alias;
    phazite::ClipNotes notes;
};
} // namespace

TEST_CASE ("Track holdings: a version's line of a track added since, holding an alias of a clip on an older track, leaves that "
           "clip no alias")
{
    AliasAcrossTracks a;
    const auto version = phazite::test::copyWholeSong (a.song);
    const auto added = a.s.addTrack ("Added");
    a.makeAlias (added);

    a.checkLeftNoAlias (
        "Restore Track",
        [&a, &version, added]
        {
            REQUIRE (phazite::makeTrackLike (a.song, added, version));
            phazite::removeBrokenReferences (a.song);
        },
        a.clip);

    const auto tracks = phazite::getTrackIDs (a.song);
    CHECK (std::find (tracks.begin(), tracks.end(), added) == tracks.end());
}

TEST_CASE ("Track holdings: a merge's choice of a whole track whose side holds its clip unlinked leaves the alias on another track "
           "no alias")
{
    AliasAcrossTracks a;
    const auto stays = a.s.addTrack ("Stays");
    const auto version = phazite::test::copyWholeSong (a.song);
    const auto then = phazite::test::findTrackIn (version, a.older);
    REQUIRE (then.isValid());
    a.makeAlias (stays);
    const auto now = phazite::test::copyWholeSong (a.song);

    // The other line's Older whole, its clip in no group: the alias on Stays is the last of its group.
    phazite::MergeClash clash;
    clash.line = "Older";
    clash.trackID = a.older.getRaw();
    clash.path = pathOf (then);
    clash.baseElement = then;
    clash.oursElement = phazite::test::findTrackIn (now, a.older);
    clash.theirsElement = then;
    const phazite::MergeSides sides { version, now, version };

    a.checkLeftNoAlias (
        "Take the Other Branch's",
        [&a, &clash, &sides] { REQUIRE (phazite::chooseTrackClash (a.song, clash, 2, sides) == phazite::AreaChoice::chosen); }, a.alias);

    REQUIRE (phazite::getClipValue (a.song, a.clip).has_value());
    CHECK (phazite::getClipValue (a.song, a.clip)->link == 0);
}

TEST_CASE ("Track holdings: a version applied whole that lacks tracks leaves nothing of them in any area, and one that moved a "
           "track out of its folder leaves it what it holds")
{
    Holdings h;

    SUBCASE ("Tracks gone")
    {
        const auto version = h.versionAfter ([&h] { phazite::deleteTracks (h.song, { h.track, h.goneReturn, h.group }, nullptr); });
        h.checkRemoval ("Bring Back",
                        [&h, &version]
                        {
                            phazite::makeSongLike (h.song, version);
                            phazite::removeBrokenReferences (h.song);
                        },
                        { h.track, h.goneReturn, h.group, h.sub, h.audio, h.inner });
    }

    SUBCASE ("A track moved out of its folder")
    {
        const auto version = h.versionAfter ([&h] { phazite::moveTracksAfter (h.song, { h.audio }, h.stays); });
        const auto sends = phazite::getSends (h.song, h.audio);
        h.checkRemoval ("Bring Back",
                        [&h, &version]
                        {
                            phazite::makeSongLike (h.song, version);
                            phazite::removeBrokenReferences (h.song);
                        },
                        {});

        phazite::redo (h.song);
        CHECK_FALSE (phazite::getParentFolder (h.song, h.audio).isValid());
        CHECK (phazite::getClips (h.song, h.audio) == std::vector { h.audioClip });
        CHECK (phazite::getSends (h.song, h.audio) == sends);
    }
}

TEST_CASE ("Track holdings: a merge's choice of a whole track gives it the side's clips, notes, modulators, macros and sends "
           "through each area's code, the keeper taking nothing in")
{
    Holdings h;
    const auto version = phazite::test::copyWholeSong (h.song);
    const auto then = phazite::test::findTrackIn (version, h.track);
    REQUIRE (then.isValid());
    const phazite::ModulatorPlace channel { .track = h.track };
    const phazite::ModulatorPlace inContainer { .track = h.track, .container = h.container };
    const auto clips = phazite::getClipValues (h.song, h.track);
    const auto notes = phazite::getClipNotesValue (h.song, h.clip);
    const auto modulators = phazite::getOwnedModulators (h.song, channel);
    const auto macros = phazite::getOwnedModulators (h.song, inContainer);
    const auto sends = phazite::getSendValues (h.song, h.track);
    REQUIRE (clips.size() == 1);
    REQUIRE (modulators.size() == 1);
    REQUIRE_FALSE (macros.empty());
    REQUIRE (sends.size() == 1);

    // This line changed the track since: another clip made, its LFO taken out, its macro turned, its send taken away (its
    // clip, in an alias group with the one on stays, left as it is: a group's notes are the group's, which a clip that joins
    // it takes).
    {
        const phazite::UndoStep step (h.song, "Change");
        REQUIRE (phazite::addMidiClipAt (h.song, h.track, phazite::test::at (16.0), nullptr).isValid());
        phazite::removeModulator (h.song, h.lfo);
        phazite::setMacroValue (h.song, h.macro, 0.75f);
        phazite::removeSend (h.song, sends[0].id);
    }

    h.song.getUndoManager().clearUndoHistory();
    const auto now = phazite::test::copyWholeSong (h.song);

    // The other line's track whole, chosen: a clash only a commit's song from anyone makes (a merge of two of Phazite's
    // keeps a track taken away on one side and changed on the other, and merges the rest by its parts).
    phazite::MergeClash clash;
    clash.line = "Track";
    clash.trackID = h.track.getRaw();
    clash.path = pathOf (then);
    clash.baseElement = then;
    clash.oursElement = phazite::test::findTrackIn (now, h.track);
    clash.theirsElement = then;
    const phazite::MergeSides sides { version, now, version };
    REQUIRE (phazite::findByPath (now, clash.path).isValid()); // the song's whole copy's: its tree holds no TRACK

    h.checkRemoval ("Take the Other Branch's", [&h, &clash, &sides]
                    { REQUIRE (phazite::chooseTrackClash (h.song, clash, 2, sides) == phazite::AreaChoice::chosen); }, {});

    phazite::redo (h.song);
    CHECK (phazite::getClipValues (h.song, h.track) == clips);
    CHECK (phazite::getClipNotesValue (h.song, h.clip) == notes);
    CHECK (phazite::getAliasClips (h.song, h.clip) == std::vector { h.clip, h.alias });
    CHECK (phazite::getOwnedModulators (h.song, channel) == modulators);
    CHECK (phazite::getOwnedModulators (h.song, inContainer) == macros);
    CHECK (phazite::getSendValues (h.song, h.track) == sends);

    // The whole track as the other line holds it: its devices too, which its element brings until the devices' switch.
    const auto chosen = phazite::test::copyTrack (h.song, h.track);
    INFO (chosen.toXmlString() << "\n" << then.toXmlString());
    CHECK (chosen.isEquivalentTo (then));

    // A path to a track's own property is the tracks' chooser's too (roadmap step S5.20, the tracks' versions pass): its field
    // the side's, through the tracks' setter (its name taken away: the side held none).
    auto property = clash;
    property.property = schema::trackName;
    {
        const phazite::UndoStep step (h.song, "Take the Other Branch's: Name");
        CHECK (phazite::chooseTrackClash (h.song, property, 2, sides) == phazite::AreaChoice::chosen);
    }

    CHECK (phazite::test::getTrackProperty (h.song, h.track, schema::trackName).isVoid());
}

TEST_CASE ("Track holdings: a merge's choice of the whole master gives it the side's modulators and its container's macros "
           "through modulation's code")
{
    Holdings h;
    const auto master = phazite::getMasterID (h.song);
    ModulatorID lfo;

    {
        const phazite::UndoStep step (h.song, "Add Modulator");
        lfo = phazite::addModulator (h.song, master, phazite::ModulatorType::lfo);
    }

    REQUIRE (lfo.isValid());

    // A container on the master, its first macro set and modulating its chain's volume (the macros reached through the
    // master's devices).
    const auto container = phazite::addContainer (h.s.context, master);
    REQUIRE (container.isValid());
    const auto chains = phazite::getChains (h.s.context, container);
    REQUIRE_FALSE (chains.empty());
    const auto masterMacros = phazite::getContainerMacros (h.song, container);
    REQUIRE_FALSE (masterMacros.empty());
    const auto masterMacro = masterMacros[0];
    const ParameterRef chainVolume { chains[0].id, schema::chainVolumeParameter };
    h.s.setMacroValue (masterMacro, 0.25f);
    h.s.modulate (masterMacro, chainVolume, -0.5f);
    h.song.getUndoManager().clearUndoHistory();
    const phazite::ModulatorPlace inContainer { .onMaster = true, .container = container };
    const auto macros = phazite::getOwnedModulators (h.song, inContainer);
    REQUIRE (macros.size() == masterMacros.size());
    REQUIRE (phazite::getModulations (h.song, masterMacro).size() == 1);

    const auto version = phazite::test::copyWholeSong (h.song);
    const auto then = version.getChildWithName (schema::MASTER);

    // This line changed the master since: its LFO taken out, its container's macro turned and its modulation taken away.
    {
        const phazite::UndoStep step (h.song, "Change");
        phazite::removeModulator (h.song, lfo);
        phazite::setMacroValue (h.song, masterMacro, 0.75f);
        phazite::removeModulation (h.song, masterMacro, chainVolume);
    }

    REQUIRE (phazite::getOwnedModulators (h.song, inContainer) != macros);

    h.song.getUndoManager().clearUndoHistory();
    const auto now = phazite::test::copyWholeSong (h.song);
    phazite::MergeClash clash;
    clash.line = "Master";
    clash.trackID = master.getRaw();
    clash.path = pathOf (then);
    clash.baseElement = then;
    clash.oursElement = now.getChildWithName (schema::MASTER);
    clash.theirsElement = then;

    h.checkRemoval ("Take the Other Branch's", [&h, &clash, &version, &now]
                    { REQUIRE (phazite::chooseTrackClash (h.song, clash, 2, { version, now, version }) == phazite::AreaChoice::chosen); },
                    {});

    phazite::redo (h.song);
    CHECK (phazite::getModulators (h.song, master) == std::vector { lfo });
    CHECK (phazite::getOwnedModulators (h.song, inContainer) == macros);
    CHECK (phazite::getContainerMacros (h.song, container) == masterMacros);
    CHECK (phazite::getModulations (h.song, masterMacro).size() == 1);
}
