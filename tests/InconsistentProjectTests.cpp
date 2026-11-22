// SPDX-License-Identifier: AGPL-3.0-or-later
// Projects and presets that parse but don't hold together, as a file from someone else could be: each opens into a song
// that passes the consistency check (SongCheck.h), with what didn't hold left out or brought within its range
// (model/project/internal/Project.h: makeConsistent()), or is refused. And the limits on what a DAWproject unpacks.
#include "GoldenFiles.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/ChordLaneValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/base/internal/WholeState.h"
#include "model/chords/internal/Chords.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/devices/internal/DrumKit.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/SongPieces.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
using namespace phazite::test;
namespace schema = phazite::schema;

// A project with something of everything a file names: tracks with an instrument, a drum sampler with a pad's sound file,
// a compressor ducking under a track, MIDI and audio clips, automation, written to its file; and that file opened again,
// edited first as someone else's program might have written it.
struct Edited
{
    TestSong s;
    phazite::TrackID lead, drums, audio;
    phazite::DeviceID synth, sampler;
    TemporaryFolder temporary;
    juce::File file = temporary.folder.getChildFile ("Edited").getChildFile ("Edited.phazite");

    Edited()
    {
        lead = s.addTrack ("Lead");
        synth = s.setInstrument (lead, phazite::devices::Synth::typeID);
        const auto clip = s.addMidiClip (lead, 0.0, 4.0);
        s.addNote (clip, 60, 0.0, 1.0);
        drums = s.addTrack ("Drums");
        sampler = s.setInstrument (drums, phazite::devices::DrumSampler::typeID);
        audio = s.addTrack ("Audio");
        s.addAudioClip (audio, writeSound (s.getSoundsFolder().getChildFile ("Loop.wav"), 0.25), 4.0);
        s.addAutomation (s.getVolumeParameter (lead), { { at (0.0), 0.5f }, { at (4.0), 0.8f } });
        REQUIRE (phazite::duckUnder (*phazite::findSong (s.context), lead, drums).isValid());
        REQUIRE (s.getProject().save().wasOk());
    }

    // The project's file, edited, written into a folder of its own with the project's Audio folder, and opened.
    std::unique_ptr<phazite::Project> open (const std::function<void (juce::XmlElement&)>& edit)
    {
        auto xml = juce::parseXML (s.getProject().getFile());
        REQUIRE (xml != nullptr);
        edit (*xml);
        file.getParentDirectory().createDirectory();
        s.getProject().getAudioFolder().copyDirectoryTo (file.getSiblingFile ("Audio"));
        REQUIRE (file.replaceWithText (xml->toString()));
        juce::String error;
        auto project = phazite::Project::open (file, false, error);

        if (project != nullptr)
            checkSong (project->getSong());

        return project;
    }

    static juce::XmlElement* findTrack (juce::XmlElement& song, const juce::String& name)
    {
        juce::XmlElement* found = nullptr;
        const std::function<void (juce::XmlElement&)> visit = [&] (juce::XmlElement& element)
        {
            for (auto* child : element.getChildIterator())
            {
                if (child->hasTagName ("TRACK") && child->getStringAttribute ("name") == name)
                    found = child;

                visit (*child);
            }
        };

        visit (song);
        REQUIRE (found != nullptr);
        return found;
    }
};

std::vector<juce::ValueTree> findAll (const juce::ValueTree& tree, const juce::Identifier& type)
{
    std::vector<juce::ValueTree> found;
    const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
    {
        for (const auto& child : element)
        {
            if (child.hasType (type))
                found.push_back (child);

            visit (child);
        }
    };

    visit (tree);
    return found;
}
} // namespace

TEST_CASE ("The project as it was saved passes the consistency check, as the golden representative project does")
{
    Edited edited;
    REQUIRE (edited.open ([] (juce::XmlElement&) {}) != nullptr);

    TemporaryFolder temporary;
    const auto file = temporary.folder.getChildFile ("Representative.phazite");
    REQUIRE (getGoldenFolder().getChildFile ("projects/representative.xml").copyFileTo (file));
    juce::String error;
    const auto project = phazite::Project::open (file, false, error);
    REQUIRE (project != nullptr);
    checkSong (project->getSong());
}

TEST_CASE ("A track's volume beyond any fader's opens at the top of its range, and one that is no number at 0 dB")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            Edited::findTrack (song, "Lead")->setAttribute ("volume", "150");
            Edited::findTrack (song, "Drums")->setAttribute ("volume", "nan");
            Edited::findTrack (song, "Drums")->setAttribute ("pan", "-7");
        });
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    CHECK (phazite::getChannelVolume (song, edited.lead) == doctest::Approx (phazite::highestVolume));
    CHECK (phazite::getChannelVolume (song, edited.drums) == doctest::Approx (0.0f));
    CHECK (phazite::getChannelPan (song, edited.drums) == doctest::Approx (-1.0f));
}

TEST_CASE ("A note's and a controller's MIDI channel open within 1 to 16, one in it as it was")
{
    // Channel 0 opens on 1, 40 on 16, one that is no number on 1, and 9 as it is; the first channel written as none, as a
    // clip's value writes it (model/base/internal/ClipNotesValue.h).
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* clip = Edited::findTrack (song, "Lead")->getChildByName ("CLIP");
            REQUIRE (clip != nullptr);
            auto* note = clip->getChildByName ("NOTE");
            REQUIRE (note != nullptr);
            note->setAttribute ("channel", "40");

            for (const auto& [channel, value] : { std::pair ("0", 1), std::pair ("nan", 2), std::pair ("9", 3) })
            {
                auto* control = clip->createNewChildElement ("CONTROL");
                control->setAttribute ("kind", "controller");
                control->setAttribute ("number", 1);
                control->setAttribute ("beat", 0.5);
                control->setAttribute ("value", value);
                control->setAttribute ("channel", channel);
            }
        });
    REQUIRE (project != nullptr);
    const auto notes = findAll (phazite::test::copyWholeSong (project->getSong()), schema::NOTE);
    REQUIRE (notes.size() == 1);
    CHECK ((int) notes[0][schema::noteChannel] == 16);

    std::map<int, juce::var> channels; // by value
    for (const auto& control : findAll (phazite::test::copyWholeSong (project->getSong()), schema::CONTROL))
        channels[(int) control[schema::controlValue]] = control[schema::controlChannel];

    REQUIRE (channels.size() == 3);
    CHECK (channels[1].isVoid());
    CHECK (channels[2].isVoid());
    CHECK ((int) channels[3] == 9);
}

TEST_CASE ("A note that lacks a pitch, a start, a length or a velocity opens with the one its description gives")
{
    // A file from others can hold a note without a property Phazite always writes. In the tree such a note stayed
    // without it, and read as 0; a clip's notes are a value now (model/base/internal/ClipNotesValue.h), which always holds
    // each, so the note opens with what the description gives for a value that is no number: a velocity of 100, a
    // pitch of 60, a length of a beat, a start at its clip's own. CI's fuzzer found a velocity of 0 written back.
    for (const auto* missing : { "pitch", "start", "length", "velocity" })
    {
        CAPTURE (missing);
        Edited edited;
        const auto project = edited.open (
            [missing] (juce::XmlElement& song)
            {
                auto* clip = Edited::findTrack (song, "Lead")->getChildByName ("CLIP");
                REQUIRE (clip != nullptr);
                auto* note = clip->getChildByName ("NOTE");
                REQUIRE (note != nullptr);
                REQUIRE (note->hasAttribute (missing));
                note->removeAttribute (missing);
            });
        REQUIRE (project != nullptr);
        checkSong (project->getSong());

        const auto notes = findAll (phazite::test::copyWholeSong (project->getSong()), schema::NOTE);
        REQUIRE_FALSE (notes.empty());
        const auto& note = notes[0];
        const juce::String name (missing);
        const auto expected = name == "pitch" ? 60.0 : name == "start" ? 0.0 : name == "length" ? 1.0 : 100.0;
        REQUIRE (note.hasProperty (juce::Identifier (name)));
        CHECK ((double) note[juce::Identifier (name)] == doctest::Approx (expected));
    }
}

TEST_CASE ("A clip that lacks a number its description gives a range, or holds one that is no number, opens with the one its "
           "description gives")
{
    // A file from others can hold a clip without a property Phazite writes, or with one that is no number (which the loader
    // takes away where its description gives no fallback). The song holds its clips as values (model/base/internal/
    // ClipsValue.h: ClipValue), which always hold each, so the clip opens with what its description gives (Schema.h): a
    // start at 0, a MIDI clip 4 beats long, its take the first; else none (an offset, a part of its file, a tempo, a pitch, a
    // gain, fades and their bends, takes of 0). Phazite writes a property only while it isn't its default (ClipsValue.h:
    // writeClipElement()), so where the clips saved hold one at its default, the clip is first given a value that is written
    // (give), and the file is required to hold it before it is taken away.
    struct Case
    {
        const char* property;
        bool audio;
        double expected;
        std::function<void (phazite::ClipValue&)> give = nullptr; // none for a property always written
    };

    const auto read = [] (const phazite::ClipValue& clip, const juce::String& name)
    {
        const std::map<juce::String, double> numbers { { "start", clip.start.inBeats() },
                                                       { "length", clip.length.inBeats() },
                                                       { "offset", clip.offset.inBeats() },
                                                       { "fileStart", clip.fileStart },
                                                       { "fileEnd", clip.fileEnd },
                                                       { "tempo", clip.sound.tempo },
                                                       { "pitch", (double) clip.sound.pitch },
                                                       { "gain", (double) clip.gain },
                                                       { "fadeIn", clip.fadeIn.seconds },
                                                       { "fadeOut", clip.fadeOut.seconds },
                                                       { "fadeInBend", (double) clip.fadeIn.bend },
                                                       { "fadeOutBend", (double) clip.fadeOut.bend },
                                                       { "takes", (double) clip.takes },
                                                       { "take", (double) clip.take },
                                                       { "takeLength", clip.takeLength } };

        return numbers.at (name);
    };

    const auto takes = [] (phazite::ClipValue& clip)
    {
        clip.takes = 3;
        clip.take = 2;
        clip.takeLength = 0.05;
    };

    const std::vector<Case> cases {
        { "start", false, 0.0 },
        { "length", false, 4.0 },
        { "offset", false, 0.0, [] (phazite::ClipValue& clip) { clip.offset = phazite::BeatDuration::fromBeats (1.0); } },
        { "start", true, 0.0 },
        { "fileStart", true, 0.0 },
        { "fileEnd", true, 0.0 },
        { "tempo", true, 0.0, [] (phazite::ClipValue& clip) { clip.sound.tempo = 120.0; } },
        { "pitch", true, 0.0, [] (phazite::ClipValue& clip) { clip.sound.pitch = 2.0f; } },
        { "gain", true, 0.0, [] (phazite::ClipValue& clip) { clip.gain = -3.0f; } },
        { "fadeIn", true, 0.0, [] (phazite::ClipValue& clip) { clip.fadeIn.seconds = 0.05; } },
        { "fadeOut", true, 0.0, [] (phazite::ClipValue& clip) { clip.fadeOut.seconds = 0.05; } },
        { "fadeInBend", true, 0.0, [] (phazite::ClipValue& clip) { clip.fadeIn.bend = 0.5f; } },
        { "fadeOutBend", true, 0.0, [] (phazite::ClipValue& clip) { clip.fadeOut.bend = 0.5f; } },
        { "takes", true, 0.0, takes },
        { "take", true, 1.0, takes },
        { "takeLength", true, 0.0, takes },
    };

    for (const auto& each : cases)
    {
        for (const auto noNumber : { false, true })
        {
            CAPTURE (each.property);
            CAPTURE (noNumber);
            Edited edited;

            if (each.give != nullptr)
            {
                auto& song = *phazite::findSong (edited.s.context);
                const auto clips = phazite::getClips (song, each.audio ? edited.audio : edited.lead);
                REQUIRE (clips.size() == 1);
                auto value = phazite::getClipValue (song, clips.front()).value();
                each.give (value);

                {
                    const phazite::UndoStep step (song, "Give");
                    phazite::setClip (song, value);
                }

                REQUIRE (edited.s.getProject().save().wasOk());
            }

            const auto project = edited.open (
                [&each, noNumber] (juce::XmlElement& song)
                {
                    auto* clip = Edited::findTrack (song, each.audio ? "Audio" : "Lead")->getChildByName ("CLIP");
                    REQUIRE (clip != nullptr);

                    if (noNumber)
                    {
                        clip->setAttribute (each.property, "not a number");
                    }
                    else
                    {
                        REQUIRE (clip->hasAttribute (each.property)); // so that taking it away takes something
                        clip->removeAttribute (each.property);
                    }
                });
            REQUIRE (project != nullptr);
            checkSong (project->getSong());

            const auto clips = phazite::getClips (project->getSong(), each.audio ? edited.audio : edited.lead);
            REQUIRE (clips.size() == 1);
            const auto clip = phazite::getClipValue (project->getSong(), clips.front());
            REQUIRE (clip.has_value());
            CHECK (read (*clip, each.property) == doctest::Approx (each.expected));
        }
    }
}

TEST_CASE ("A clip where no track holds clips, and a note outside a MIDI clip, are left out")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* clip = Edited::findTrack (song, "Lead")->getChildByName ("CLIP");
            REQUIRE (clip != nullptr);
            song.getChildByName ("TRACKS")->addChildElement (new juce::XmlElement (*clip)); // on no track
            song.getChildByName ("MASTER")->addChildElement (new juce::XmlElement (*clip)); // on the master
            Edited::findTrack (song, "Drums")->addChildElement (new juce::XmlElement (*clip->getChildByName ("NOTE")));
        });
    REQUIRE (project != nullptr);
    CHECK (findAll (phazite::test::copyWholeSong (project->getSong()), schema::CLIP).size()
           == 2); // the MIDI clip and the audio clip, each once
    CHECK (findAll (phazite::test::copyWholeSong (project->getSong()), schema::NOTE).size() == 1);
}

TEST_CASE ("A clip on a return or a folder is left out, as only a track holds clips")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* clip = Edited::findTrack (song, "Lead")->getChildByName ("CLIP");
            REQUIRE (clip != nullptr);
            Edited::findTrack (song, "Audio")->setAttribute ("kind", "return"); // with its audio clip
            auto* folder = song.getChildByName ("TRACKS")->createNewChildElement ("TRACK");
            folder->setAttribute ("id", "777780");
            folder->setAttribute ("kind", "folder");
            folder->addChildElement (new juce::XmlElement (*clip));
        });
    REQUIRE (project != nullptr);
    const auto whole = phazite::test::copyWholeSong (project->getSong()); // kept: the clips' parents are its elements
    const auto clips = findAll (whole, schema::CLIP);
    REQUIRE (clips.size() == 1); // the lead's MIDI clip alone
    CHECK (clips.front().getParent()[schema::trackName].toString() == juce::String ("Lead"));
}

TEST_CASE ("A clip anywhere but on a track, in an element Phazite doesn't know too, is left out of the song and of its tree")
{
    // The loader takes a CLIP out of every element it knows that holds none (a device here), but leaves the insides of an
    // element of a kind it doesn't know as they are (Project.cpp, makeConsistent()): the song, which holds its clips as
    // values, takes those out of its tree as it opens (ClipsArea.cpp, adopt()), so that its tree holds no CLIP at all.
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* lead = Edited::findTrack (song, "Lead");
            auto* clip = lead->getChildByName ("CLIP");
            REQUIRE (clip != nullptr);
            auto* device = lead->getChildByName ("DEVICE");
            REQUIRE (device != nullptr);
            // In an element Phazite knows; on a track, in one it doesn't; on the song, the same.
            device->addChildElement (new juce::XmlElement (*clip));
            lead->createNewChildElement ("FUTURE")->addChildElement (new juce::XmlElement (*clip));
            song.createNewChildElement ("ELSEWHERE")->addChildElement (new juce::XmlElement (*clip));
        });
    REQUIRE (project != nullptr); // open() checks the song: no CLIP in its tree
    auto& song = project->getSong();
    CHECK (findAll (song.getState(), schema::CLIP).empty());
    CHECK (phazite::getAllClips (song).size() == 2); // the MIDI clip and the audio clip, each once
    CHECK (phazite::getClips (song, edited.lead).size() == 1);

    // What Phazite doesn't know stays, without the clip.
    CHECK (phazite::findTrackState (song, edited.lead).getChildWithName ("FUTURE").isValid());
    CHECK (song.getState().getChildWithName ("ELSEWHERE").isValid());
}

TEST_CASE ("A clip whose properties don't hold together opens as the clips' own edits keep one, and is the same opened again")
{
    // Each property within its range, as the loader keeps it, but not with the others: a file's end before its start, a MIDI
    // clip shorter than the shortest, a take past its count of takes, a take without takes. The song takes each in as the
    // clips' setters keep a value (ClipsValue.h: keepWithinClip()), so what it holds is a clip's value, which a save writes
    // and an open reads back as it was.
    struct Case
    {
        const char* what;
        bool audio;
        std::function<void (juce::XmlElement& clip)> edit;
        std::function<void (const phazite::ClipValue&)> check;
    };

    const std::vector<Case> cases {
        { "a file's end before its start", true,
          [] (juce::XmlElement& clip)
          {
              clip.setAttribute ("fileStart", 0.2);
              clip.setAttribute ("fileEnd", 0.1);
          },
          [] (const phazite::ClipValue& clip)
          {
              CHECK (clip.fileStart == doctest::Approx (0.2));
              CHECK (clip.fileEnd == doctest::Approx (0.2));
          } },
        { "a MIDI clip shorter than the shortest", false, [] (juce::XmlElement& clip) { clip.setAttribute ("length", 0.001); },
          [] (const phazite::ClipValue& clip) { CHECK (clip.length.inBeats() == doctest::Approx (phazite::shortestClipBeats)); } },
        { "a take past its count", true,
          [] (juce::XmlElement& clip)
          {
              clip.setAttribute ("takes", 3);
              clip.setAttribute ("take", 7);
              clip.setAttribute ("takeLength", 0.05);
          },
          [] (const phazite::ClipValue& clip)
          {
              CHECK (clip.takes == 3);
              CHECK (clip.take == 3);
          } },
        { "a take without takes", true, [] (juce::XmlElement& clip) { clip.setAttribute ("take", 2); },
          [] (const phazite::ClipValue& clip)
          {
              CHECK (clip.takes == 0);
              CHECK (clip.take == 1);
          } },
    };

    for (const auto& each : cases)
    {
        CAPTURE (each.what);
        Edited edited;
        const auto track = each.audio ? edited.audio : edited.lead;
        const auto read = [track] (phazite::Project& project)
        {
            const auto clips = phazite::getClips (project.getSong(), track);
            REQUIRE (clips.size() == 1);
            return phazite::getClipValue (project.getSong(), clips.front()).value();
        };

        auto project = edited.open (
            [&each] (juce::XmlElement& song)
            {
                auto* clip = Edited::findTrack (song, each.audio ? "Audio" : "Lead")->getChildByName ("CLIP");
                REQUIRE (clip != nullptr);
                each.edit (*clip);
            });
        REQUIRE (project != nullptr);
        const auto opened = read (*project);
        CHECK (phazite::keepWithinClip (opened) == opened);
        each.check (opened);

        // Saved, and opened again: the same.
        REQUIRE (project->save().wasOk());
        project = nullptr;
        juce::String error;
        const auto again = phazite::Project::open (edited.file, false, error);
        REQUIRE (again != nullptr);
        checkSong (again->getSong());
        CHECK (read (*again) == opened);
    }
}

TEST_CASE ("Two tracks with one ID both open, each with an ID of its own")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* drums = Edited::findTrack (song, "Drums");
            drums->setAttribute ("id", Edited::findTrack (song, "Lead")->getStringAttribute ("id"));
        });
    REQUIRE (project != nullptr);
    CHECK (phazite::getTrackIDs (project->getSong()).size() == phazite::getTrackIDs (*phazite::findSong (edited.s.context)).size());
}

TEST_CASE ("A number where no item's ID is, too large to count on from, leaves the song counting from its own IDs")
{
    // The fuzzer's finding, with many rounds (the nightly run, 2026-11-15): "id" on the TRACKS element, which holds none,
    // was counted as the song's largest ID, and the next item made got one past any the song can count on from.
    Edited edited;
    const auto project =
        edited.open ([] (juce::XmlElement& song) { song.getChildByName ("TRACKS")->setAttribute ("id", "-9223372036854775808"); });
    REQUIRE (project != nullptr);
    const auto made = project->getSong().makeID();
    CHECK (made > 0);
    CHECK (made < phazite::schema::maxID);
}

TEST_CASE ("The loader's checks alone leave the song's count of IDs past every ID it counts, as the song counts them")
{
    // "link", "fader" and "vca" on a note, which another program's file may put there, hold no ID: the song counted them as
    // IDs (Schema.h: findLargestID()) and makeConsistent() didn't, and left the count below one until its callers counted
    // again. Now neither does.
    auto made = phazite::createSong();
    const auto track = phazite::ensureTracks (*made, 1).front();
    const auto clip = addMidiClip (*made, track, { at (0.0), at (4.0) }, "Notes");
    addMidiNote (*made, clip, 60, at (0.0), lasting (1.0), 100);

    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, phazite::test::copyWholeSong (*made));
    auto xml = juce::parseXML (text.toString());
    REQUIRE (xml != nullptr);
    xml->setAttribute ("nextID", "10");
    auto* note = xml->getChildByName ("TRACKS")->getChildByName ("TRACK")->getChildByName ("CLIP")->getChildByName ("NOTE");
    REQUIRE (note != nullptr);
    note->setAttribute ("link", "900000");

    phazite::Song song (juce::ValueTree::fromXml (*xml));
    phazite::makeConsistent (song);
    const auto counted = (std::uint64_t) (juce::int64) song.getState()[schema::nextID];
    CHECK (phazite::findNextID (song.getState()) <= counted);
    CHECK (counted < 900000);
}

TEST_CASE ("An ID too near the last a song can give opens given anew, and what is added after it is still there at the next opening")
{
    // A track's ID just below schema::maxID: counted on from, the song's next ID was maxID itself, so that every item made
    // after it opened had an ID past what a song can hold, given anew at the next opening, with what named it (the send to a
    // return, the lane and curve of a fader) left out. Now a song whose IDs reach schema::renumberMark is given every ID at
    // or past maxCountedID anew, once, what named it (here the compressor ducking under the drums) naming the new one; any
    // other keeps its IDs, those Phazite makes after it opened too, however its tracks and devices are ordered.
    struct Case
    {
        std::uint64_t id;
        bool givenAnew;
    };

    for (const auto [id, givenAnew] :
         { Case { phazite::schema::maxID - 1, true }, Case { phazite::schema::renumberMark, true },
           Case { phazite::schema::renumberMark - 1000000, false }, Case { phazite::schema::maxCountedID + 5, false },
           Case { phazite::schema::maxCountedID - 1, false } })
    {
        INFO (juce::String ((juce::int64) id));
        Edited edited;
        auto project = edited.open (
            [id] (juce::XmlElement& song)
            {
                auto* key = Edited::findTrack (song, "Drums");
                const auto old = key->getStringAttribute ("id");
                key->setAttribute ("id", juce::String ((juce::int64) id));
                auto ducked = 0;
                const std::function<void (juce::XmlElement&)> rename = [&] (juce::XmlElement& element)
                {
                    for (auto* child : element.getChildIterator())
                    {
                        if (child->getStringAttribute ("sidechain") == old)
                        {
                            child->setAttribute ("sidechain", juce::String ((juce::int64) id));
                            ++ducked;
                        }

                        rename (*child);
                    }
                };

                rename (song);
                REQUIRE (ducked == 1);
            });
        REQUIRE (project != nullptr);
        auto& song = project->getSong();
        const auto next = (std::uint64_t) (juce::int64) song.getState()[schema::nextID];
        CHECK (next < phazite::schema::renumberMark);

        if (givenAnew)
            CHECK (next < phazite::schema::maxCountedID); // counted on from below 2^61, the file's own IDs being small
        else
            CHECK (next == id + 1);

        // Stamped with its new numbering where its IDs were given anew (Schema.h: idNumbering), else not.
        CHECK (song.getState().hasProperty (schema::idNumbering) == givenAnew);

        // By their names, as the IDs given anew aren't known here.
        const auto findNamed = [] (const phazite::Song& s, const juce::String& name)
        {
            for (const auto each : phazite::getTrackIDs (s))
                if (phazite::getTrackName (s, each) == name)
                    return each;

            return phazite::TrackID();
        };

        const auto findKey = [&findNamed] (const phazite::Song& s) { return findNamed (s, "Drums"); };

        const auto findDucker = [] (const phazite::Song& s)
        {
            const auto devices = findAll (s.getState(), schema::DEVICE);
            const auto found = std::find_if (devices.begin(), devices.end(),
                                             [] (const juce::ValueTree& device) { return device.hasProperty (schema::deviceSidechain); });
            return found != devices.end() ? phazite::readID ((*found)[schema::deviceSidechain]) : 0;
        };

        REQUIRE (findKey (song).isValid());
        CHECK (findDucker (song) == findKey (song).getRaw());
        CHECK ((findKey (song).getRaw() == id) == ! givenAnew);

        // Added after it opened: a track with a send to a new return and its volume's lane and automation; then the tracks
        // reordered and a device added to an earlier track, so that the order of the song's elements isn't the order its
        // IDs were made in.
        const auto track = phazite::addTrackAtEnd (song, false, nullptr);
        phazite::setTrackName (song, track, "Added");
        const auto returnTrack = phazite::addReturnTrack (song, "Return", nullptr);
        REQUIRE (phazite::addSend (song, track, returnTrack, false).isValid());
        const phazite::ParameterRef volume { phazite::getFader (song, track), phazite::getVolumeParameterID (song, track) };
        phazite::showLane (song, track, volume);
        REQUIRE (phazite::addPoint (song, volume, { at (1.0), 0.5f }) >= 0);
        phazite::moveTracksAfter (song, { findNamed (song, "Lead") }, track);
        REQUIRE (phazite::insertDeviceState (song, findNamed (song, "Audio"),
                                             phazite::makeOwnDeviceState (song, phazite::devices::Compressor::typeID))
                     .isValid());
        REQUIRE (project->save().wasOk());

        // Opened again, twice: all of it there, and each save the same as the one made before it opened.
        const auto file = project->getFile();
        project = nullptr;
        auto previous = file.loadFileAsString();

        for (int opening = 0; opening < 2; ++opening)
        {
            juce::String error;
            const auto reopened = phazite::Project::open (file, false, error);
            REQUIRE (reopened != nullptr);
            auto& again = reopened->getSong();
            checkSong (again);
            const auto added = findNamed (again, "Added"), returned = findNamed (again, "Return");
            REQUIRE (added.isValid());
            REQUIRE (returned.isValid());
            const auto sends = findAll (phazite::findTrackState (again, added), schema::SEND);
            REQUIRE (sends.size() == 1);
            CHECK (phazite::readID (sends.front()[schema::sendTo]) == returned.getRaw());
            CHECK (findAll (phazite::findTrackState (again, added), schema::LANE).size() == 1);
            const phazite::ParameterRef volumeAgain { phazite::getFader (again, added), phazite::getVolumeParameterID (again, added) };
            CHECK (phazite::getPoints (again, volumeAgain).size() == 1);
            CHECK (findDucker (again) == findKey (again).getRaw());
            CHECK ((std::uint64_t) (juce::int64) again.getState()[schema::nextID] < phazite::schema::maxID);

            REQUIRE (reopened->save().wasOk());
            const auto text = file.loadFileAsString();
            CHECK (text == previous);
            previous = text;
        }
    }
}

namespace
{
// The tracks' and their clips' elements of a song, in order: a track's by its name.
juce::ValueTree findNamedTrack (const phazite::Song& song, const juce::String& name)
{
    for (const auto& track : findAll (song.getState(), schema::TRACK))
        if (track[schema::trackName].toString() == name)
            return track;

    return {};
}
} // namespace

TEST_CASE ("Aliased clips whose notes' IDs are given anew as the song opens still follow each other")
{
    // The lead's clip and an alias of it, their alias group and their notes' IDs past schema::renumberMark: each note given
    // the same new ID in both clips, as an edit of a note of one finds its twin in the other by its ID.
    Edited edited;
    const auto mark = (juce::int64) phazite::schema::renumberMark;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* clip = Edited::findTrack (song, "Lead")->getChildByName ("CLIP");
            REQUIRE (clip != nullptr);
            clip->setAttribute ("link", juce::String (mark + 50));

            for (auto* note : clip->getChildWithTagNameIterator ("NOTE"))
                note->setAttribute ("id", juce::String (mark + 7));

            auto* alias = new juce::XmlElement (*clip);
            alias->setAttribute ("id", juce::String (mark + 60));
            alias->setAttribute ("start", "8");
            Edited::findTrack (song, "Lead")->addChildElement (alias);
        });
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    const auto clips =
        phazite::getClipValues (song, phazite::TrackID::fromRaw (phazite::readID (findNamedTrack (song, "Lead")[schema::trackID])));
    REQUIRE (clips.size() == 2);
    CHECK (clips[0].link == clips[1].link);
    CHECK (clips[0].link < phazite::schema::maxCountedID); // given anew, from below 2^61

    const auto first = clips[0].id;
    const auto second = clips[1].id;
    const auto notes = phazite::getClipNotes (song, first);
    REQUIRE (notes.size() == 1);
    REQUIRE (phazite::getClipNotes (song, second).size() == 1);
    CHECK (phazite::getClipNotes (song, second).front().id == notes.front().id);

    // An edit of the one, the other the same.
    phazite::moveNote (song, notes.front().getRef(), notes.front().start, 72);
    CHECK (phazite::getClipNotes (song, second).front().pitch == 72);
}

TEST_CASE ("A track given a new ID as the song opens isn't frozen, as another track's frozen file can have its new name")
{
    // Frozen files are named by their tracks' IDs (model/mixer/internal/Freeze.h): a track given a new ID would play the file
    // of the track that had that ID. Two frozen tracks with one ID, and two whose IDs reach schema::renumberMark, each with a
    // frozen file of every ID the song would give anew.
    for (const auto pastTheMark : { false, true })
    {
        INFO (juce::String (pastTheMark ? "past the mark" : "an ID twice"));
        Edited edited;
        const auto project = edited.open (
            [&edited, pastTheMark] (juce::XmlElement& song)
            {
                auto* lead = Edited::findTrack (song, "Lead");
                auto* drums = Edited::findTrack (song, "Drums");
                const auto mark = (juce::int64) phazite::schema::renumberMark;

                if (pastTheMark)
                {
                    lead->setAttribute ("id", juce::String (mark + 1));
                    drums->setAttribute ("id", juce::String (mark + 2));
                }
                else
                {
                    drums->setAttribute ("id", lead->getStringAttribute ("id"));
                }

                lead->setAttribute ("frozen", "1");
                drums->setAttribute ("frozen", "1");

                // The files: of the IDs the song holds, and of every one it could give anew.
                const auto cache = phazite::Project::getCacheFolder (edited.file);
                REQUIRE (cache.createDirectory());

                for (juce::int64 id = 1; id < 500; ++id)
                    REQUIRE (cache.getChildFile ("Frozen " + juce::String (id) + ".wav").replaceWithText ("RIFF"));
            });
        REQUIRE (project != nullptr);
        auto& song = project->getSong();
        const auto lead = findNamedTrack (song, "Lead"), drums = findNamedTrack (song, "Drums");

        if (pastTheMark)
        {
            CHECK_FALSE ((bool) lead[schema::trackFrozen]);
            CHECK_FALSE ((bool) drums[schema::trackFrozen]);
        }
        else
        {
            CHECK ((bool) lead[schema::trackFrozen]); // the first keeps its ID, and its file
            CHECK_FALSE ((bool) drums[schema::trackFrozen]);
        }
    }
}

TEST_CASE ("A name of no item, past the IDs a song counts on from, is left out as the song's IDs are given anew")
{
    // The compressor's sidechain names 2^61, which no item holds, in a song given its IDs anew from just below 2^61: the
    // drums, past 2^61, get 2^61 itself, and the sidechain mustn't follow them there.
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            const auto counted = (juce::int64) phazite::schema::maxCountedID;
            auto* drums = Edited::findTrack (song, "Drums");
            const auto old = drums->getStringAttribute ("id");
            Edited::findTrack (song, "Lead")->setAttribute ("id", juce::String (counted - 1));
            drums->setAttribute ("id", juce::String (counted + 3));
            Edited::findTrack (song, "Audio")->setAttribute ("id", juce::String ((juce::int64) phazite::schema::renumberMark));
            song.setAttribute ("nextID", "1");
            auto renamed = 0;
            const std::function<void (juce::XmlElement&)> rename = [&] (juce::XmlElement& element)
            {
                for (auto* child : element.getChildIterator())
                {
                    if (child->getStringAttribute ("sidechain") == old)
                    {
                        child->setAttribute ("sidechain", juce::String (counted));
                        ++renamed;
                    }

                    rename (*child);
                }
            };

            rename (song);
            REQUIRE (renamed == 1);
        });
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    CHECK (phazite::readID (findNamedTrack (song, "Drums")[schema::trackID]) == phazite::schema::maxCountedID);

    for (const auto& device : findAll (song.getState(), schema::DEVICE))
        CHECK_FALSE (device.hasProperty (schema::deviceSidechain));
}

TEST_CASE ("A file with an ID just below the mark opens as it is, and what its session made is given anew once, at the next opening")
{
    // The one way across the mark, accepted (Schema.h: renumberMark): the drums' ID just below it, so that the song's next
    // ID is the mark itself and all the session makes is past it. The next opening gives it anew, once; the song keeps it all.
    Edited edited;
    const auto mark = (juce::int64) phazite::schema::renumberMark;
    auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* drums = Edited::findTrack (song, "Drums");
            const auto old = drums->getStringAttribute ("id");
            drums->setAttribute ("id", juce::String (mark - 1));
            auto renamed = 0;
            const std::function<void (juce::XmlElement&)> rename = [&] (juce::XmlElement& element)
            {
                for (auto* child : element.getChildIterator())
                {
                    if (child->getStringAttribute ("sidechain") == old)
                    {
                        child->setAttribute ("sidechain", juce::String (mark - 1));
                        ++renamed;
                    }

                    rename (*child);
                }
            };

            rename (song);
            REQUIRE (renamed == 1);
        });
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    CHECK (phazite::readID (findNamedTrack (song, "Drums")[schema::trackID]) == (std::uint64_t) (mark - 1)); // as it is
    CHECK (phazite::readID (song.getState()[schema::nextID]) == phazite::schema::renumberMark);
    CHECK_FALSE (song.getState().hasProperty (schema::idNumbering)); // nothing given anew yet

    // Made in the session, past the mark: a track with a send to a new return, and its volume's lane and automation.
    const auto track = phazite::addTrackAtEnd (song, false, nullptr);
    phazite::setTrackName (song, track, "Added");
    const auto returnTrack = phazite::addReturnTrack (song, "Return", nullptr);
    REQUIRE (phazite::addSend (song, track, returnTrack, false).isValid());
    const phazite::ParameterRef volume { phazite::getFader (song, track), phazite::getVolumeParameterID (song, track) };
    phazite::showLane (song, track, volume);
    REQUIRE (phazite::addPoint (song, volume, { at (1.0), 0.5f }) >= 0);
    CHECK (track.getRaw() >= phazite::schema::renumberMark);
    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    // Opened and closed without a save, as often as it is: given anew the same way, with the same stamp each time
    // (Schema.h: idNumbering), so that what was kept of it in one session matches it in the next.
    const auto openUnsaved = [&file]
    {
        juce::String error;
        const auto opened = phazite::Project::open (file, false, error);
        REQUIRE (opened != nullptr);
        return std::make_pair (opened->getSong().getState()[schema::idNumbering].toString(),
                               phazite::readID (findNamedTrack (opened->getSong(), "Added")[schema::trackID]));
    };

    const auto unsaved = openUnsaved();
    CHECK (unsaved == openUnsaved());
    juce::String afterFirst;

    for (int opening = 0; opening < 2; ++opening)
    {
        juce::String error;
        const auto reopened = phazite::Project::open (file, false, error);
        REQUIRE (reopened != nullptr);
        auto& again = reopened->getSong();
        checkSong (again);

        // Stamped with its new numbering at the opening that gave its IDs anew, and the same at every opening after.
        const auto stampNow = again.getState()[schema::idNumbering].toString();
        CHECK (stampNow.length() == phazite::schema::idNumberingDigits);
        CHECK (stampNow.containsOnly ("0123456789abcdef"));
        CHECK (stampNow == unsaved.first);

        // Given anew, once: every ID at or past 2^61, the drums' and the session's, from below 2^61.
        const auto drums = findNamedTrack (again, "Drums"), added = findNamedTrack (again, "Added"),
                   returned = findNamedTrack (again, "Return");
        REQUIRE (drums.isValid());
        REQUIRE (added.isValid());
        REQUIRE (returned.isValid());
        CHECK (phazite::readID (drums[schema::trackID]) < phazite::schema::maxCountedID);
        CHECK (phazite::readID (added[schema::trackID]) < phazite::schema::maxCountedID);
        CHECK (phazite::readID (added[schema::trackID]) == unsaved.second); // as at the openings without a save
        CHECK (phazite::readID (again.getState()[schema::nextID]) < phazite::schema::maxCountedID);

        // All of it there, what named each naming it.
        const auto sends = findAll (added, schema::SEND);
        REQUIRE (sends.size() == 1);
        CHECK (phazite::readID (sends.front()[schema::sendTo]) == phazite::readID (returned[schema::trackID]));
        CHECK (findAll (added, schema::LANE).size() == 1);
        const auto addedID = phazite::TrackID::fromRaw (phazite::readID (added[schema::trackID]));
        const phazite::ParameterRef volumeAgain { phazite::getFader (again, addedID), phazite::getVolumeParameterID (again, addedID) };
        CHECK (phazite::getPoints (again, volumeAgain).size() == 1);
        const auto devices = findAll (again.getState(), schema::DEVICE);
        CHECK (std::any_of (devices.begin(), devices.end(), [&drums] (const juce::ValueTree& device)
                            { return phazite::readID (device[schema::deviceSidechain]) == phazite::readID (drums[schema::trackID]); }));

        // And the same at every opening after the first.
        REQUIRE (reopened->save().wasOk());
        const auto text = file.loadFileAsString();

        if (opening == 0)
            afterFirst = text;
        else
            CHECK (text == afterFirst);
    }
}

namespace
{
// Every attribute that holds or names an ID, of an element and those in it, whose value is one text, set to another.
int renameValues (juce::XmlElement& element, const juce::String from,
                  const juce::String& to) // from by value: it may be one of the attributes renamed
{
    static const juce::StringArray naming { "id", "fader", "vca", "link", "device", "sidechain", "to" };
    auto renamed = 0;

    for (int i = 0; i < element.getNumAttributes(); ++i)
        if (naming.contains (element.getAttributeName (i)) && element.getAttributeValue (i) == from)
        {
            element.setAttribute (element.getAttributeName (i), to);
            ++renamed;
        }

    for (auto* child : element.getChildIterator())
        renamed += renameValues (*child, from, to);

    return renamed;
}
} // namespace

TEST_CASE ("A song's numbering that isn't 16 lowercase hexadecimal digits is left out as the file opens, and one that is stays")
{
    // Schema.h: idNumbering. Text of any other form names no numbering: too short, too long, a capital letter, not hex.
    Edited edited;
    const auto name = schema::idNumbering.toString();

    for (const auto* wrong : { "0123456789abcde", "0123456789abcdef0", "0123456789aBcdef", "0123456789abcdeg", "" })
    {
        INFO (wrong);
        const auto project = edited.open ([&] (juce::XmlElement& song) { song.setAttribute (name, wrong); });
        REQUIRE (project != nullptr);
        CHECK_FALSE (project->getSong().getState().hasProperty (schema::idNumbering));
    }

    const auto project = edited.open ([&] (juce::XmlElement& song) { song.setAttribute (name, "0123456789abcdef"); });
    REQUIRE (project != nullptr);
    CHECK (project->getSong().getState()[schema::idNumbering].toString() == "0123456789abcdef");
}

TEST_CASE ("An ID past 2^61 inside an element Phazite doesn't know neither raises the song's count nor gives its IDs anew")
{
    // The loader keeps such an element as it is and gives nothing in it anew, so the song mustn't count it either: counted,
    // it made the song give its IDs anew at every opening, or put its next ID at maxID, and every commit of it refused.
    for (const auto id : { phazite::schema::maxID - 1, phazite::schema::renumberMark, phazite::schema::maxCountedID + 5 })
    {
        INFO (juce::String ((juce::int64) id));
        Edited edited;
        auto project = edited.open (
            [id] (juce::XmlElement& song)
            {
                auto* future = song.createNewChildElement ("FUTURE");
                future->createNewChildElement ("TRACK")->setAttribute ("id", juce::String ((juce::int64) id));
            });
        REQUIRE (project != nullptr);
        auto& song = project->getSong();
        CHECK (phazite::readID (song.getState()[schema::nextID]) < phazite::schema::maxCountedID);
        CHECK (phazite::readID (findNamedTrack (song, "Lead")[schema::trackID]) == edited.lead.getRaw()); // nothing given anew

        const auto track = phazite::addTrackAtEnd (song, false, nullptr);
        phazite::setTrackName (song, track, "Added");
        REQUIRE (project->save().wasOk());
        const auto file = project->getFile();
        project = nullptr;
        auto previous = file.loadFileAsString();

        for (int opening = 0; opening < 2; ++opening)
        {
            juce::String error;
            const auto reopened = phazite::Project::open (file, false, error);
            REQUIRE (reopened != nullptr);
            checkSong (reopened->getSong());
            CHECK (findNamedTrack (reopened->getSong(), "Added").isValid());
            const auto future = reopened->getSong().getState().getChildWithName ("FUTURE");
            CHECK (phazite::readID (future.getChild (0)[schema::trackID]) == id); // kept as it is
            REQUIRE (reopened->save().wasOk());
            const auto text = file.loadFileAsString();
            CHECK (text == previous);
            previous = text;
        }
    }
}

TEST_CASE ("IDs given anew as the song opens are counted on from past what a name of a deleted device still names")
{
    // A curve of a device deleted stays in the file and goes as the song opens. A song given its IDs anew, its saved count
    // past 2^61 left out, counted on from the largest ID it holds below 2^61: the synth, past the mark, got the ID the
    // curve names, and the deleted device's automation played on the synth.
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            const auto mark = (juce::int64) phazite::schema::renumberMark;
            auto* synth = Edited::findTrack (song, "Lead")->getChildByName ("DEVICE");
            REQUIRE (synth != nullptr);
            REQUIRE (renameValues (song, synth->getStringAttribute ("id"), juce::String (mark)) >= 1);
            song.setAttribute ("nextID", juce::String (mark));

            // The largest ID it holds now below 2^61, and a curve of the deleted device that had the next one.
            juce::int64 largest = 0;
            const std::function<void (const juce::XmlElement&)> find = [&] (const juce::XmlElement& element)
            {
                for (const auto* name : { "id", "fader", "vca", "link" })
                    if (const auto value = element.getStringAttribute (name).getLargeIntValue();
                        value < (juce::int64) phazite::schema::maxCountedID)
                        largest = std::max (largest, value);

                for (const auto* child : element.getChildIterator())
                    find (*child);
            };

            find (song);
            auto* curve = song.getChildByName ("AUTOMATION")->createNewChildElement ("CURVE");
            curve->setAttribute ("device", juce::String (largest + 1));
            curve->setAttribute ("parameter", "cutoff");
            auto* point = curve->createNewChildElement ("POINT");
            point->setAttribute ("beat", "0");
            point->setAttribute ("value", "1000");
        });
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    const auto synth = findNamedTrack (song, "Lead").getChildWithName (schema::DEVICE);
    REQUIRE (synth.isValid());

    for (const auto& curve : phazite::getCurveKeys (song))
        CHECK (curve.device.getRaw() != phazite::readID (synth[schema::deviceID]));
}

TEST_CASE ("Every merge waiting goes from a song given its IDs anew as it opens")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            Edited::findTrack (song, "Audio")->setAttribute ("id", juce::String ((juce::int64) phazite::schema::renumberMark));

            for (int i = 0; i < 2; ++i)
                song.createNewChildElement ("MERGING")->setAttribute ("chosen", "1");
        });
    REQUIRE (project != nullptr);
    CHECK_FALSE (project->getSong().getState().getChildWithName (schema::MERGING).isValid());
}

TEST_CASE ("A macro, and the master's fader and VCA, past the mark, are given anew, the master volume's curve following")
{
    // The master holds its fader's and its VCA's IDs as a track does; and a macro is an item. (A curve of a macro's parameter,
    // which no Phazite makes, goes as every song opens: Automation.h, forgetCurvesOfGoneDevices().)
    Edited edited;
    auto& made = *phazite::findSong (edited.s.context);
    const auto macro = phazite::addMacro (made, phazite::findTrackState (made, edited.lead), "Macro");
    REQUIRE (macro.isValid());
    const auto master = phazite::getMasterID (made);
    REQUIRE (
        phazite::addPoint (made, { phazite::getFader (made, master), phazite::getVolumeParameterID (made, master) }, { at (1.0), 0.5f })
        >= 0);
    REQUIRE (edited.s.getProject().save().wasOk());

    const auto project = edited.open (
        [macro] (juce::XmlElement& song)
        {
            const auto markHere = (juce::int64) phazite::schema::renumberMark;
            const auto macroID = juce::String ((juce::int64) macro.getRaw());
            REQUIRE (renameValues (song, macroID, juce::String (markHere + 3)) == 1);

            auto* masterElement = song.getChildByName ("TRACKS")->getChildByName ("MASTER");

            if (masterElement == nullptr)
                masterElement = song.getChildByName ("MASTER");

            REQUIRE (masterElement != nullptr);
            REQUIRE (renameValues (song, masterElement->getStringAttribute ("fader"), juce::String (markHere + 10)) >= 2);
            masterElement->setAttribute ("vca", juce::String (markHere + 11));
        });
    REQUIRE (project != nullptr);
    auto& song = project->getSong();

    // The macro, given anew.
    const auto macros = findAll (findNamedTrack (song, "Lead"), schema::MACRO);
    REQUIRE (macros.size() == 1);
    const auto newMacro = phazite::readID (macros.front()[schema::modulatorID]);
    CHECK (newMacro < phazite::schema::maxCountedID);

    // The master's fader and VCA, given anew, its volume's curve following.
    const auto masterID = phazite::getMasterID (song);
    const auto masterState = phazite::findTrackState (song, masterID);
    const auto fader = phazite::DeviceID::fromRaw (phazite::readID (masterState[schema::channelFader])); // not its VCA
    CHECK (fader.getRaw() < phazite::schema::maxCountedID);
    CHECK (phazite::readID (masterState[schema::channelVCA]) < phazite::schema::maxCountedID);
    CHECK (phazite::getPoints (song, { fader, phazite::schema::masterVolume }).size() == 1);
}

TEST_CASE ("A device of a type Phazite hasn't is left out, with what names it")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* device = Edited::findTrack (song, "Lead")->getChildByName ("DEVICE");
            REQUIRE (device != nullptr);
            device->setAttribute ("type", "someoneElsesSynth");
        });
    REQUIRE (project != nullptr);
    CHECK_FALSE (phazite::findDeviceState (project->getSong(), edited.synth).isValid());
}

TEST_CASE ("A tempo of zero opens at the slowest tempo, one that is no number at 120 BPM, and a time signature of 0/3 at a real one")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* map = song.getChildByName ("TEMPOMAP");
            REQUIRE (map != nullptr);
            map->getChildByName ("TEMPO")->setAttribute ("bpm", "0");
            auto* second = new juce::XmlElement ("TEMPO");
            second->setAttribute ("beat", "8");
            second->setAttribute ("bpm", "inf");
            map->addChildElement (second);
            map->getChildByName ("TIMESIGNATURE")->setAttribute ("numerator", "0");
            map->getChildByName ("TIMESIGNATURE")->setAttribute ("denominator", "3");
        });
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    CHECK (phazite::getTempo (song) == doctest::Approx (20.0));
    const auto signature = phazite::getTimeSignature (song);
    CHECK (signature.numerator == 1);
    CHECK (signature.denominator == 4);
}

namespace
{
// A song's tempo map as its value holds it (TempoMapValue.h), each entry as text: a tempo's beat, bpm and curve, a time
// signature's beat, numerator and denominator.
std::vector<juce::String> describeTempoMap (const phazite::Song& song)
{
    std::vector<juce::String> entries;
    const auto& map = phazite::getTempoMapValue (song);

    for (const auto& tempo : map.tempos)
        entries.push_back ("tempo " + juce::String (tempo.beat.inBeats(), 2) + " " + juce::String (tempo.bpm, 2) + " "
                           + juce::String (tempo.curve, 2));

    for (const auto& signature : map.signatures)
        entries.push_back ("signature " + juce::String (signature.beat.inBeats(), 2) + " " + juce::String (signature.numerator) + "/"
                           + juce::String (signature.denominator));

    return entries;
}
} // namespace

TEST_CASE ("A damaged tempo map opens as a whole value, each entry as its description reads it, saved and opened again the same")
{
    // The song takes in its file's tempo map as it opens (Song::holdValues(): TempoMapValue.h, readHeldTempoMap()): an entry
    // without a bpm or a beat, with a number that is none, a map with no entry, one without a time signature, and what Phazite
    // doesn't know on the map and its entries. Each opens holding together (the check, in Edited::open()), holds what its
    // description reads, and is the same once saved and opened again, its TEMPOMAP written tempos first, then time
    // signatures, without what Phazite doesn't know.
    struct Case
    {
        const char* about;
        std::function<void (juce::XmlElement&)> edit;
        std::vector<juce::String> expected;
    };

    const auto mapOf = [] (juce::XmlElement& song) -> juce::XmlElement&
    {
        auto* map = song.getChildByName ("TEMPOMAP");
        REQUIRE (map != nullptr);
        return *map;
    };

    const Case cases[] = {
        { "a tempo without a bpm",
          [&mapOf] (juce::XmlElement& song)
          {
              auto* tempo = mapOf (song).createNewChildElement ("TEMPO");
              tempo->setAttribute ("beat", "8");
          },
          { "tempo 0.00 120.00 1.00", "tempo 8.00 120.00 1.00", "signature 0.00 4/4" } },
        { "a tempo without a beat",
          [&mapOf] (juce::XmlElement& song)
          {
              auto* tempo = mapOf (song).getChildByName ("TEMPO");
              tempo->removeAttribute ("beat");
              tempo->setAttribute ("bpm", "100");
          },
          { "tempo 0.00 100.00 1.00", "signature 0.00 4/4" } },
        { "a bpm that is no number",
          [&mapOf] (juce::XmlElement& song) { mapOf (song).getChildByName ("TEMPO")->setAttribute ("bpm", "fast"); },
          { "tempo 0.00 120.00 1.00", "signature 0.00 4/4" } },
        { "a map with no entry",
          [&mapOf] (juce::XmlElement& song) { mapOf (song).deleteAllChildElements(); },
          { "tempo 0.00 120.00 1.00", "signature 0.00 4/4" } },
        { "a map without a time signature",
          [&mapOf] (juce::XmlElement& song)
          {
              auto& map = mapOf (song);
              map.getChildByName ("TEMPO")->setAttribute ("bpm", "90");
              map.removeChildElement (map.getChildByName ("TIMESIGNATURE"), true);
          },
          { "tempo 0.00 90.00 1.00", "signature 0.00 4/4" } },
        { "what Phazite doesn't know, and a time signature before a tempo",
          [&mapOf] (juce::XmlElement& song)
          {
              auto& map = mapOf (song);
              map.setAttribute ("colour", "red");
              map.getChildByName ("TEMPO")->setAttribute ("swing", "0.2");
              map.createNewChildElement ("LINE");
              auto* signature = new juce::XmlElement ("TIMESIGNATURE");
              signature->setAttribute ("beat", "16");
              signature->setAttribute ("numerator", "3");
              signature->setAttribute ("denominator", "4");
              map.prependChildElement (signature);
          },
          { "tempo 0.00 120.00 1.00", "signature 0.00 4/4", "signature 16.00 3/4" } },
    };

    for (const auto& each : cases)
    {
        INFO (juce::String (each.about));
        Edited edited;
        const auto project = edited.open (each.edit);
        REQUIRE (project != nullptr);
        CHECK (describeTempoMap (project->getSong()) == each.expected);

        const auto map = phazite::test::copyWholeSong (project->getSong()).getChildWithName (schema::TEMPOMAP);
        CHECK (map.getNumProperties() == 0);

        for (const auto& child : map)
        {
            CHECK ((child.hasType (schema::TEMPO) || child.hasType (schema::TIMESIGNATURE)));
            CHECK_FALSE (child.hasProperty ("swing"));
        }

        REQUIRE (project->save().wasOk());
        juce::String error;
        const auto again = phazite::Project::open (edited.file, false, error);
        REQUIRE_MESSAGE (again != nullptr, error.toStdString());
        checkSong (again->getSong());
        CHECK (describeTempoMap (again->getSong()) == each.expected);
        CHECK (phazite::test::copyWholeSong (again->getSong()).getChildWithName (schema::TEMPOMAP).isEquivalentTo (map));
    }
}

TEST_CASE ("A damaged chord lane opens as a whole value, each entry as its description reads it, saved and opened again the same")
{
    // The song takes in its file's chord lane as it opens (Song::holdValues(): ChordLaneValue.h, readHeldChordLane()): a chord
    // without a start, a length or a degree, numbers that are none or outside their ranges, a key without a root, keys before
    // chords, an empty lane, and what Phazite doesn't know on the lane and its entries. Each opens holding together (the
    // check, in Edited::open()), holds what its description reads, and is the same once saved and opened again, its CHORDLANE
    // written chords first, then keys, without what Phazite doesn't know. An empty lane stays a lane.
    struct Case
    {
        const char* about;
        const char* lane;
        std::vector<juce::String> expected;
    };

    const auto describe = [] (const phazite::Song& song)
    {
        std::vector<juce::String> lines;

        if (! phazite::hasChordLane (song))
            return lines;

        lines.push_back ("lane");
        const auto lane = phazite::getChordLaneValue (song);

        for (const auto& chord : lane.chords)
            lines.push_back ("chord " + juce::String (chord.start.inBeats(), 2) + " " + juce::String (chord.length.inBeats(), 2) + " "
                             + juce::String (chord.degree) + " " + juce::String (chord.accidental) + " " + juce::String (chord.inversion)
                             + " " + chord.type);

        for (const auto& key : lane.keys)
            lines.push_back ("key " + juce::String (key.start.inBeats(), 2) + " " + juce::String (key.root) + " " + key.scale);

        return lines;
    };

    const Case cases[] = {
        { "a chord without a start, a length or a degree",
          R"(<CHORDLANE><CHORD type="major"/></CHORDLANE>)",
          { "lane", "chord 0.00 4.00 1 0 0 major" } },
        { "numbers that are none, and beyond their ranges",
          R"(<CHORDLANE><CHORD start="8" length="fast" degree="12" accidental="-3" inversion="40" type="minor"/></CHORDLANE>)",
          { "lane", "chord 8.00 4.00 7 -1 16 minor" } },
        { "a key without a root, and keys before chords",
          R"(<CHORDLANE><CHORDKEY start="0" keyRoot="2" keyScale="minor"/><CHORDKEY start="8" keyScale="major"/>)"
          R"(<CHORD start="0" length="8" degree="1" type="minor"/></CHORDLANE>)",
          { "lane", "chord 0.00 8.00 1 0 0 minor", "key 0.00 2 minor", "key 8.00 -1 major" } },
        { "an empty lane", R"(<CHORDLANE/>)", { "lane" } },
        { "what Phazite doesn't know",
          R"(<CHORDLANE colour="red"><LINE/><CHORD start="4" length="4" degree="5" type="major" swing="0.2"/></CHORDLANE>)",
          { "lane", "chord 4.00 4.00 5 0 0 major" } },
    };

    for (const auto& each : cases)
    {
        INFO (juce::String (each.about));
        Edited edited;
        const auto project = edited.open (
            [&each] (juce::XmlElement& song)
            {
                auto lane = juce::parseXML (each.lane);
                REQUIRE (lane != nullptr);
                song.addChildElement (lane.release());
            });
        REQUIRE (project != nullptr);
        CHECK (describe (project->getSong()) == each.expected);

        const auto lane = phazite::test::copyWholeSong (project->getSong()).getChildWithName (schema::CHORDLANE);
        REQUIRE (lane.isValid());
        CHECK (lane.getNumProperties() == 0);
        auto keys = false;

        for (const auto& child : lane)
        {
            CHECK ((child.hasType (schema::CHORD) || child.hasType (schema::CHORDKEY)));
            CHECK_FALSE (child.hasProperty ("swing"));
            CHECK_FALSE ((keys && child.hasType (schema::CHORD))); // chords first, then keys
            keys = keys || child.hasType (schema::CHORDKEY);
        }

        REQUIRE (project->save().wasOk());
        juce::String error;
        const auto again = phazite::Project::open (edited.file, false, error);
        REQUIRE_MESSAGE (again != nullptr, error.toStdString());
        checkSong (again->getSong());
        CHECK (describe (again->getSong()) == each.expected);
        CHECK (phazite::test::copyWholeSong (again->getSong()).getChildWithName (schema::CHORDLANE).isEquivalentTo (lane));
    }
}

TEST_CASE ("A chord lane whose chords overlap opens with its chords apart, as the lane's writers leave them, saved and opened again "
           "the same")
{
    // The song takes in its file's chord lane with its chords apart (ChordLaneValue.h: readHeldChordLane(), keepChordsApart()):
    // a chord reaches no further than the next one's start, one that would then have no length goes, and one without a
    // length (4 beats long, its description's fallback) before another ends where the other starts. Of two at one start the
    // loader has kept the first already (makeConsistent(): no two elements with one key). The chords that stay keep the
    // file's order. Each opens holding
    // together (the check, in Edited::open(), which asks that chords don't overlap) and is the same once saved and opened
    // again.
    struct Case
    {
        const char* about;
        const char* lane;
        std::vector<juce::String> expected;
    };

    const auto describe = [] (const phazite::Song& song)
    {
        std::vector<juce::String> lines;

        for (const auto& chord : phazite::getChordLaneValue (song).chords)
            lines.push_back (juce::String (chord.start.inBeats(), 2) + " " + juce::String (chord.length.inBeats(), 2) + " " + chord.type);

        return lines;
    };

    const Case cases[] = {
        { "chords that overlap",
          R"(<CHORDLANE><CHORD start="0" length="8" type="major"/><CHORD start="4" length="4" type="minor"/></CHORDLANE>)",
          { "0.00 4.00 major", "4.00 4.00 minor" } },
        { "a chord without a length before another",
          R"(<CHORDLANE><CHORD start="0" type="major"/><CHORD start="2" length="4" type="minor"/></CHORDLANE>)",
          { "0.00 2.00 major", "2.00 4.00 minor" } },
        { "a chord without a length after another, in the file's order",
          R"(<CHORDLANE><CHORD start="4" length="4" type="minor"/><CHORD start="2" type="major"/></CHORDLANE>)",
          { "4.00 4.00 minor", "2.00 2.00 major" } },
        { "two chords at one start: the first, as the loader keeps it (no two of a lane's chords with one start)",
          R"(<CHORDLANE><CHORD start="0" length="4" type="major"/><CHORD start="0" length="2" type="minor"/></CHORDLANE>)",
          { "0.00 4.00 major" } },
        { "a chord that would be left no length by the next one's start",
          R"(<CHORDLANE><CHORD start="0" length="4" type="major"/><CHORD start="0.0000009" length="2" type="minor"/></CHORDLANE>)",
          { "0.00 2.00 minor" } },
        { "a chord over two others",
          R"(<CHORDLANE><CHORD start="0" length="16" type="major"/><CHORD start="4" length="2" type="minor"/>)"
          R"(<CHORD start="8" length="4" type="major"/></CHORDLANE>)",
          { "0.00 4.00 major", "4.00 2.00 minor", "8.00 4.00 major" } },
        { "chords that touch",
          R"(<CHORDLANE><CHORD start="0" length="4" type="major"/><CHORD start="4" length="4" type="minor"/></CHORDLANE>)",
          { "0.00 4.00 major", "4.00 4.00 minor" } },
    };

    for (const auto& each : cases)
    {
        INFO (juce::String (each.about));
        Edited edited;
        const auto project = edited.open (
            [&each] (juce::XmlElement& song)
            {
                auto lane = juce::parseXML (each.lane);
                REQUIRE (lane != nullptr);
                song.addChildElement (lane.release());
            });
        REQUIRE (project != nullptr);
        CHECK (describe (project->getSong()) == each.expected);
        CHECK (phazite::holdsChordsApart (phazite::getChordLaneValue (project->getSong())));
        const auto lane = phazite::test::copyWholeSong (project->getSong()).getChildWithName (schema::CHORDLANE);

        REQUIRE (project->save().wasOk());
        const auto firstSave = edited.file.loadFileAsString();
        juce::String error;
        const auto again = phazite::Project::open (edited.file, false, error);
        REQUIRE_MESSAGE (again != nullptr, error.toStdString());
        checkSong (again->getSong());
        CHECK (describe (again->getSong()) == each.expected);
        CHECK (phazite::test::copyWholeSong (again->getSong()).getChildWithName (schema::CHORDLANE).isEquivalentTo (lane));
        REQUIRE (again->save().wasOk());
        CHECK (edited.file.loadFileAsString() == firstSave);
    }
}

TEST_CASE ("A number written as text reads as the same number wherever the song reads it: a project saved, opened and saved again "
           "is the same file")
{
    // The loader read "1e-321" as that subnormal and the readers of the song's values read the text Phazite writes for it
    // ("9.98012604599318e-322") as 0 (juce::String::getDoubleValue()), so the second save wrote "0" (the fuzzer found it for
    // the tempo map). Every reader of an element's number into a value reads text as the loader does (SongText.h:
    // readValueNumber()). Each text below in a MIDI clip's start, its note's start, a controller's value's position, an
    // audio clip's warp marker's time and an automation point's beat. What each reads as, the loader's rule: a number's text
    // as that number ("1e-321" as that subnormal where the library reads one, " 4 " trimmed as 4); what is no finite number
    // as each property's fallback, 0 for all of these: "1e-400" (too near 0 for a double: out of range, as infinity), "0x10"
    // and "4abc" (not a number and nothing else: none), "nan" and "inf" (no finite number).
    using phazite::readValueNumber;
    CHECK (readValueNumber (juce::var ("1e-321")).value_or (0.0) > 0.0);
    CHECK (std::isinf (readValueNumber (juce::var ("1e-400")).value_or (0.0)));
    CHECK_FALSE (readValueNumber (juce::var ("0x10")).has_value());
    CHECK (juce::exactlyEqual (readValueNumber (juce::var (" 4 ")).value_or (0.0), 4.0));
    CHECK_FALSE (readValueNumber (juce::var ("4abc")).has_value());
    CHECK (std::isnan (readValueNumber (juce::var ("nan")).value_or (0.0)));
    CHECK (std::isinf (readValueNumber (juce::var ("inf")).value_or (0.0)));

    // The numbers of the song's values, as its whole copy writes them (from the values, as doubles).
    const auto readNumbers = [] (const phazite::Song& song)
    {
        const auto whole = phazite::test::copyWholeSong (song);
        std::vector<double> numbers;
        const auto first = [&whole] (const juce::Identifier& type, const juce::Identifier& parentType, const juce::String& kind)
        {
            for (const auto& element : findAll (whole, type))
                if (element.getParent().hasType (parentType)
                    && (kind.isEmpty() || element.getParent()[schema::clipKind].toString() == kind))
                    return element;

            return juce::ValueTree();
        };

        for (const auto& clip : findAll (whole, schema::CLIP))
            if (clip[schema::clipKind].toString() == schema::clipIsMidi)
                numbers.push_back ((double) clip[schema::clipStart]);

        numbers.push_back ((double) first (schema::NOTE, schema::CLIP, {})[schema::noteStart]);
        numbers.push_back ((double) first (schema::CONTROL, schema::CLIP, {})[schema::controlBeat]);
        numbers.push_back ((double) first (schema::WARP, schema::CLIP, schema::clipIsAudio)[schema::warpTime]);
        // A curve's points as whole state writes them from the song's values (roadmap step S5.20): read as the song's values
        // read one.
        numbers.push_back (readValueNumber (first (schema::POINT, schema::CURVE, {})[schema::pointBeat]).value_or (-1.0));
        return numbers;
    };

    const char* const names[] = { "the MIDI clip's start", "the note's start", "the controller's value's position",
                                  "the warp marker's time", "the automation point's beat" };

    for (const auto* text : { "1e-321", "1e-400", "0x10", " 4 ", "4abc", "nan", "inf" })
    {
        const auto quoted = juce::String ("the text \"") + text + "\"";
        INFO (quoted);
        const auto read = readValueNumber (juce::var (text));
        const auto expected = read.has_value() && std::isfinite (*read) ? *read : 0.0;

        Edited edited;
        const auto project = edited.open (
            [text] (juce::XmlElement& song)
            {
                auto* midi = Edited::findTrack (song, "Lead")->getChildByName ("CLIP");
                REQUIRE (midi != nullptr);
                midi->setAttribute ("start", text);
                midi->getChildByName ("NOTE")->setAttribute ("start", text);
                auto* control = midi->createNewChildElement ("CONTROL");
                control->setAttribute ("kind", schema::controlIsController);
                control->setAttribute ("number", 1);
                control->setAttribute ("beat", text);
                control->setAttribute ("value", 64);

                auto* sound = Edited::findTrack (song, "Audio")->getChildByName ("CLIP");
                REQUIRE (sound != nullptr);
                auto* warp = sound->createNewChildElement ("WARP");
                warp->setAttribute ("time", text);
                warp->setAttribute ("playsAt", 0);

                auto* automation = song.getChildByName ("AUTOMATION");
                REQUIRE (automation != nullptr);
                automation->getChildByName ("CURVE")->getChildByName ("POINT")->setAttribute ("beat", text);
            });
        REQUIRE (project != nullptr);

        const auto numbers = readNumbers (project->getSong());
        REQUIRE (numbers.size() == 5);

        for (size_t i = 0; i < numbers.size(); ++i)
            CHECK_MESSAGE (juce::exactlyEqual (numbers[i], expected), juce::String (names[i]) << " reads as " << numbers[i]);

        REQUIRE (project->save().wasOk());
        const auto firstSave = edited.file.loadFileAsString();

        juce::String error;
        const auto again = phazite::Project::open (edited.file, false, error);
        REQUIRE_MESSAGE (again != nullptr, error.toStdString());
        checkSong (again->getSong());

        const auto reopened = readNumbers (again->getSong());
        REQUIRE (reopened.size() == 5);

        for (size_t i = 0; i < reopened.size(); ++i)
            CHECK_MESSAGE (juce::exactlyEqual (reopened[i], expected), juce::String (names[i])
                                                                           << " reads as " << reopened[i] << " opened again");

        REQUIRE (again->save().wasOk());
        CHECK (edited.file.loadFileAsString() == firstSave);
    }

    // The tempo map as its lenient reader reads one (TempoMapValue.h: readTempoMapValue(), which keeps nothing within a range):
    // the same number for the same text.
    for (const auto* text : { "1e-321", "1e-400", "0x10", " 4 ", "4abc", "nan", "inf" })
    {
        const auto quoted = juce::String ("the text \"") + text + "\"";
        INFO (quoted);
        const auto read = readValueNumber (juce::var (text));
        juce::ValueTree map (schema::TEMPOMAP);
        map.appendChild (juce::ValueTree (schema::TEMPO, { { schema::mapBeat, 0.0 }, { schema::bpm, 120.0 } }), nullptr);
        map.appendChild (juce::ValueTree (schema::TEMPO, { { schema::mapBeat, text }, { schema::bpm, 130.0 } }), nullptr);
        const auto tempos = phazite::readTempoMapValue (map).tempos;
        REQUIRE (tempos.size() == 2);
        CHECK (juce::exactlyEqual (tempos.back().beat.inBeats(), read.has_value() && std::isfinite (*read) ? *read : 0.0));
    }
}

TEST_CASE ("A chord's start and a key change's written as text read as the same number as the loader reads them: a project saved, "
           "opened and saved again is the same file")
{
    // As the other areas' numbers above: the chord lane's conversion reads a number's text as the loader does (SongText.h:
    // readValueNumber()), so that "1e-321" stays that subnormal through two saves, and what is no finite number reads as the
    // start's fallback, 0.
    using phazite::readValueNumber;

    for (const auto* text : { "1e-321", "1e-400", "0x10", " 4 ", "4abc", "nan", "inf" })
    {
        const auto quoted = juce::String ("the text \"") + text + "\"";
        INFO (quoted);
        const auto read = readValueNumber (juce::var (text));
        const auto expected = read.has_value() && std::isfinite (*read) ? *read : 0.0;

        Edited edited;
        const auto project = edited.open (
            [text] (juce::XmlElement& song)
            {
                auto* lane = song.createNewChildElement ("CHORDLANE");
                auto* chord = lane->createNewChildElement ("CHORD");
                chord->setAttribute ("start", text);
                chord->setAttribute ("length", 2);
                chord->setAttribute ("degree", 1);
                chord->setAttribute ("type", "major");
                auto* key = lane->createNewChildElement ("CHORDKEY");
                key->setAttribute ("start", text);
                key->setAttribute ("keyRoot", 2);
                key->setAttribute ("keyScale", "minor");
            });
        REQUIRE (project != nullptr);

        const auto starts = [] (const phazite::Song& song)
        {
            const auto lane = phazite::getChordLaneValue (song);
            REQUIRE (lane.chords.size() == 1);
            REQUIRE (lane.keys.size() == 1);
            return std::pair (lane.chords[0].start.inBeats(), lane.keys[0].start.inBeats());
        };

        CHECK (juce::exactlyEqual (starts (project->getSong()).first, expected));
        CHECK (juce::exactlyEqual (starts (project->getSong()).second, expected));
        REQUIRE (project->save().wasOk());
        const auto firstSave = edited.file.loadFileAsString();

        juce::String error;
        const auto again = phazite::Project::open (edited.file, false, error);
        REQUIRE_MESSAGE (again != nullptr, error.toStdString());
        checkSong (again->getSong());
        CHECK (juce::exactlyEqual (starts (again->getSong()).first, expected));
        CHECK (juce::exactlyEqual (starts (again->getSong()).second, expected));
        REQUIRE (again->save().wasOk());
        CHECK (edited.file.loadFileAsString() == firstSave);

        // And as a lane is read where no loader read it first (ChordLaneValue.h: as the song holds one, a version's or a
        // merge's, and as a song that keeps everything in its tree reads one), the same number for the same text.
        juce::ValueTree lane (schema::CHORDLANE);
        lane.appendChild (juce::ValueTree (schema::CHORD, { { schema::chordStart, text }, { schema::chordLength, 2.0 } }), nullptr);

        for (const auto& value : { phazite::readHeldChordLane (lane), phazite::readChordLaneValue (lane) })
        {
            REQUIRE (value.chords.size() == 1);
            CHECK (juce::exactlyEqual (value.chords[0].start.inBeats(), expected));
        }
    }
}

TEST_CASE (
    "A sound file on a network share, or leaving the project with .., is looked for in its Audio folder; another computer's full path stays")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* clip = Edited::findTrack (song, "Audio")->getChildByName ("CLIP");
            REQUIRE (clip != nullptr);
            clip->setAttribute ("file", "C:/Users/someone/Music/Kick.wav");
            auto* copy = new juce::XmlElement (*clip);
            copy->setAttribute ("id", "999990");
            copy->setAttribute ("file", "\\\\fuzz.invalid\\share\\Snare.wav");
            Edited::findTrack (song, "Audio")->addChildElement (copy);
            auto* outside = new juce::XmlElement (*clip);
            outside->setAttribute ("id", "999991");
            outside->setAttribute ("file", "../../../Hat.wav");
            Edited::findTrack (song, "Audio")->addChildElement (outside);

            auto* sounds = Edited::findTrack (song, "Drums")->getChildByName ("DEVICE")->getChildByName ("SOUNDS");

            if (sounds == nullptr)
                sounds = Edited::findTrack (song, "Drums")->getChildByName ("DEVICE")->createNewChildElement ("SOUNDS");

            sounds->setAttribute ("pad36SoundFile", "/home/someone/Clap.wav");
        });
    REQUIRE (project != nullptr);
    juce::StringArray files;

    for (const auto& clip : findAll (phazite::test::copyWholeSong (project->getSong()), schema::CLIP))
        if (clip[schema::clipKind].toString() == schema::clipIsAudio)
            files.add (clip[schema::clipFile].toString());

    files.sort (false);
    INFO (files.joinIntoString (", "));
    CHECK (files == juce::StringArray { "Audio/Hat.wav", "Audio/Snare.wav", "C:/Users/someone/Music/Kick.wav" });
    CHECK (findAll (project->getSong().getState(), schema::SOUNDS).front()["pad36SoundFile"].toString() == "/home/someone/Clap.wav");
}

TEST_CASE ("A curve, a lane and a sidechain of what the song hasn't are left out, and a point's value comes within its parameter's range")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* automation = song.getChildByName ("AUTOMATION");
            REQUIRE (automation != nullptr);
            auto* curve = automation->getFirstChildElement();
            auto* gone = new juce::XmlElement (*curve);
            gone->setAttribute ("device", "777777");
            automation->addChildElement (gone);
            curve->getFirstChildElement()->setAttribute ("value", "12");

            const std::function<void (juce::XmlElement&)> visit = [&] (juce::XmlElement& element)
            {
                for (auto* child : element.getChildIterator())
                {
                    if (child->hasTagName ("DEVICE") && child->hasAttribute ("sidechain"))
                        child->setAttribute ("sidechain", "888888");

                    visit (*child);
                }
            };

            visit (song);
        });
    REQUIRE (project != nullptr);
    const auto curves = findAll (phazite::test::copyWholeSong (project->getSong()), schema::CURVE); // wherever they are
    REQUIRE (curves.size() == 1);
    REQUIRE (curves.front().getNumChildren() > 0);
    CHECK ((float) curves.front().getChild (0)[schema::pointValue] == doctest::Approx (1.0f));
}

TEST_CASE ("Two curves of one parameter, its device's ID written two ways, open as one, the first")
{
    // "14" and "0014" name one device (readID()): the lane would show the first while the engine played the last.
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* automation = song.getChildByName ("AUTOMATION");
            REQUIRE (automation != nullptr);
            REQUIRE (automation->getNumChildElements() == 1);
            auto* curve = automation->getFirstChildElement();
            auto* again = new juce::XmlElement (*curve);
            again->setAttribute ("device", "00" + curve->getStringAttribute ("device"));

            for (auto* point : again->getChildIterator())
                point->setAttribute ("value", "0.25");

            automation->addChildElement (again);
        });
    REQUIRE (project != nullptr);
    const auto curves = findAll (phazite::test::copyWholeSong (project->getSong()), schema::CURVE);
    REQUIRE (curves.size() == 1);
    REQUIRE (curves.front().getNumChildren() == 2);
    CHECK ((float) curves.front().getChild (0)[schema::pointValue] == doctest::Approx (0.5f));
    CHECK ((float) curves.front().getChild (1)[schema::pointValue] == doctest::Approx (0.8f));
}

TEST_CASE ("A curve's child Phazite doesn't know goes as the song opens, so that its first point is the first its edits find")
{
    // The readers count a curve's points, and the edits its children: with the child kept, the lane's first point was the
    // child to a drag or a delete.
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* automation = song.getChildByName ("AUTOMATION");
            REQUIRE (automation != nullptr);
            auto* curve = automation->getFirstChildElement();
            REQUIRE (curve != nullptr);
            auto* shape = new juce::XmlElement ("SHAPE");
            shape->setAttribute ("kind", "spline");
            curve->insertChildElement (shape, 0);
        });
    REQUIRE (project != nullptr);
    auto& song = project->getSong();

    const auto curves = findAll (phazite::test::copyWholeSong (song), schema::CURVE);
    REQUIRE (curves.size() == 1);
    REQUIRE (curves.front().getNumChildren() == 2);
    CHECK (curves.front().getChild (0).hasType (schema::POINT));
    CHECK (curves.front().getChild (1).hasType (schema::POINT));

    const auto keys = phazite::getCurveKeys (song);
    REQUIRE (keys.size() == 1);
    const auto parameter = keys.front();
    REQUIRE (phazite::getPoints (song, parameter).size() == 2);

    phazite::test::UndoCheck undo (song);
    const auto inStep = [&undo, &song] (const char* name, const std::function<void()>& edit)
    {
        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (song, name);
                        edit();
                    });
    };

    inStep ("Move Point", [&] { phazite::movePoints (song, parameter, { 0 }, phazite::BeatDuration::fromBeats (1.0), 0.1f); });
    auto points = phazite::getPoints (song, parameter);
    REQUIRE (points.size() == 2);
    CHECK (points[0].beat.inBeats() == doctest::Approx (1.0));
    CHECK (points[0].position == doctest::Approx (0.6f));
    CHECK (points[1].beat.inBeats() == doctest::Approx (4.0));
    CHECK (points[1].position == doctest::Approx (0.8f));

    inStep ("Delete Point", [&] { phazite::deletePoints (song, parameter, { 0 }); });
    points = phazite::getPoints (song, parameter);
    REQUIRE (points.size() == 1);
    CHECK (points[0].beat.inBeats() == doctest::Approx (4.0));
    CHECK (points[0].position == doctest::Approx (0.8f));
    undo.walkBackAndForth();
}

TEST_CASE ("A point without a position, or with one that is no number, is left out, and a curve left without points goes")
{
    // A point at 0 would be silence on a volume's lane: one that says no position is no point. A curve that held only
    // elements Phazite doesn't know stays without them, as a curve without points: a curve holds its points alone.
    const auto openWith = [] (const std::function<void (juce::XmlElement & curve)>& edit)
    {
        Edited edited;
        const auto project = edited.open (
            [&edit] (juce::XmlElement& song)
            {
                auto* automation = song.getChildByName ("AUTOMATION");
                REQUIRE (automation != nullptr);
                REQUIRE (automation->getNumChildElements() == 1);
                edit (*automation->getFirstChildElement());
            });
        REQUIRE (project != nullptr);
        return findAll (phazite::test::copyWholeSong (project->getSong()), schema::CURVE); // as its file would hold them
    };

    SUBCASE ("A point without a position")
    {
        const auto curves = openWith ([] (juce::XmlElement& curve) { curve.getFirstChildElement()->removeAttribute ("value"); });
        REQUIRE (curves.size() == 1);
        REQUIRE (curves.front().getNumChildren() == 1);
        CHECK ((float) curves.front().getChild (0)[schema::pointValue] == doctest::Approx (0.8f));
    }

    SUBCASE ("A point whose position is no number")
    {
        const auto curves = openWith ([] (juce::XmlElement& curve) { curve.getFirstChildElement()->setAttribute ("value", "loud"); });
        REQUIRE (curves.size() == 1);
        REQUIRE (curves.front().getNumChildren() == 1);
        CHECK ((float) curves.front().getChild (0)[schema::pointValue] == doctest::Approx (0.8f));
    }

    SUBCASE ("A curve left without points goes")
    {
        const auto curves = openWith (
            [] (juce::XmlElement& curve)
            {
                for (auto* point : curve.getChildIterator())
                    point->setAttribute ("value", "nan");
            });
        CHECK (curves.empty());
    }

    SUBCASE ("A curve holding only what Phazite doesn't know stays, without it")
    {
        const auto curves = openWith (
            [] (juce::XmlElement& curve)
            {
                curve.deleteAllChildElements();
                curve.createNewChildElement ("SHAPE")->setAttribute ("kind", "spline");
            });
        REQUIRE (curves.size() == 1);
        CHECK (curves.front().getNumChildren() == 0);
    }
}

TEST_CASE ("A device on a drum sampler's track listening to one of its own pads' tracks opens without that sidechain")
{
    // Known bug 164: the model refuses such a sidechain (its sound would come round to itself: Sidechain.h, isFedBy()), but
    // makeConsistent() left one a file brought. The lead's compressor ducking under the drums, which is no loop, stays.
    Edited edited;
    auto& song = *phazite::findSong (edited.s.context);
    const auto pad = phazite::givePadItsOwnTrack (song, edited.sampler, 36);
    REQUIRE (pad.isValid());
    const auto glue = edited.s.addEffect (edited.drums, phazite::devices::Compressor::typeID);
    const auto sources = phazite::getSidechainSources (song, glue);
    CHECK (std::find (sources.begin(), sources.end(), pad) == sources.end());
    REQUIRE (edited.s.getProject().save().wasOk());

    const auto project = edited.open (
        [&] (juce::XmlElement& xml)
        {
            const std::function<void (juce::XmlElement&)> visit = [&] (juce::XmlElement& element)
            {
                for (auto* child : element.getChildIterator())
                {
                    if (child->hasTagName ("DEVICE") && child->getStringAttribute ("id") == juce::String ((juce::int64) glue.getRaw()))
                        child->setAttribute ("sidechain", juce::String ((juce::int64) pad.getRaw()));

                    visit (*child);
                }
            };

            visit (xml);
        });
    REQUIRE (project != nullptr);
    CHECK_FALSE (phazite::getSidechainSource (project->getSong(), glue).isValid());
    const auto devices = findAll (project->getSong().getState(), schema::DEVICE);
    CHECK (std::any_of (devices.begin(), devices.end(),
                        [] (const juce::ValueTree& device) { return device.hasProperty (schema::deviceSidechain); }));
}

TEST_CASE ("A number with an exponent of ten digits or more opens as one past every number, before it is read; a name so stays")
{
    // Known bug 170: JUCE reads an exponent's digits into an int without a check (CharacterFunctions::readDoubleValue()),
    // which every number read from a song's XML goes through. Such a clip's length opens at its fallback, as one too large to
    // hold does; its start 0; the track named so keeps its name.
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* lead = Edited::findTrack (song, "Lead");
            lead->setAttribute ("name", "1e12345678901");
            auto* clip = lead->getChildByName ("CLIP");
            REQUIRE (clip != nullptr);
            clip->setAttribute ("length", "1e12345678901");
            clip->setAttribute ("start", "-2.5E-98765432109");
            lead->setAttribute ("volume", juce::String ("1e") + juce::String::charToString ((juce::juce_wchar) 0x0669));
        });
    REQUIRE (project != nullptr);
    const auto tracks = findAll (phazite::test::copyWholeSong (project->getSong()), schema::TRACK); // whole: with its clips
    const auto lead = std::find_if (tracks.begin(), tracks.end(),
                                    [] (const juce::ValueTree& track) { return track[schema::trackName].toString() == "1e12345678901"; });
    REQUIRE (lead != tracks.end());
    const auto clip = lead->getChildWithName (schema::CLIP);
    CHECK ((double) clip[schema::clipLength] == doctest::Approx (4.0));
    CHECK ((double) clip[schema::clipStart] == 0.0);
    CHECK_FALSE (lead->hasProperty (schema::channelVolume)); // past every number: none, the fader's 0 dB

    // A step sequencer's steps and the sampler's slices, numbers between spaces, each the same; nothing else changes.
    juce::ValueTree modulator (schema::MODULATOR);
    modulator.setProperty (schema::modulatorSteps, "0.5  1e12345678901 -1", nullptr);
    modulator.setProperty (schema::modulatorType, "1e12345678901", nullptr);
    juce::ValueTree song (schema::PHAZITE);
    song.getOrCreateChildWithName (schema::TRACKS, nullptr).appendChild (juce::ValueTree (schema::TRACK), nullptr);
    song.getChildWithName (schema::TRACKS).getChild (0).appendChild (modulator, nullptr);
    phazite::makeExponentsReadable (song);
    CHECK (modulator[schema::modulatorSteps].toString() == "0.5  1e999 -1");
    CHECK (modulator[schema::modulatorType].toString() == "1e12345678901");
    modulator.setProperty (schema::modulatorSteps, "0.5  1", nullptr);
    phazite::makeExponentsReadable (song);
    CHECK (modulator[schema::modulatorSteps].toString() == "0.5  1"); // as Phazite wrote it
}

TEST_CASE ("A step sequencer's steps open within -1 to 1, what is no finite number among them at 0; a Phazite file's as they are")
{
    // A reviewer, of known bug 170's fix: a step "1e12345678901" became "1e999", read as an infinity, which the engine took
    // as the step's value. The steps had no range as the song loaded.
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* modulator = Edited::findTrack (song, "Lead")->createNewChildElement ("MODULATOR");
            modulator->setAttribute ("id", "999999");
            modulator->setAttribute ("type", "steps");
            modulator->setAttribute ("steps", "0.5 1e12345678901 -3 nan 0.25");
        });
    REQUIRE (project != nullptr);
    const auto modulators = findAll (project->getSong().getState(), schema::MODULATOR);
    REQUIRE (modulators.size() == 1);
    // Those changed written as Phazite writes a step (juce::String of the number), the others as they were.
    CHECK (modulators.front()[schema::modulatorSteps].toString()
           == "0.5 " + juce::String (0.0) + " " + juce::String (-1.0) + " " + juce::String (0.0) + " 0.25");
}

TEST_CASE ("A version's piece with a number whose exponent JUCE's reader would overflow is joined readable, then made consistent")
{
    // Known bug 170, for the versions: a piece is anyone's, as a project file is. Its numbers made readable first, then the
    // song made consistent as a project file is as it opens (known bug 126's round: joinSong()).
    const auto piece =
        juce::String (R"(<PHAZITE><TRACKS><TRACK id="5" kind="track" name="1e12345678901" volume="1e12345678901" pan="5e-12345678901">)"
                      R"(<DEVICE id="7" type="phaziteSampler"><SOUNDS sampleSlices="0.25 1e12345678901"/></DEVICE>)"
                      R"(<CLIP id="6" kind="midi" start="2" length="-3e-12345678901"/></TRACK></TRACKS></PHAZITE>)");
    const auto song = phazite::joinSong ("song", [&piece] (const juce::String&) -> std::optional<juce::MemoryBlock>
                                         { return juce::MemoryBlock (piece.toRawUTF8(), piece.getNumBytesAsUTF8()); });
    REQUIRE (song.isValid());
    const auto track = song.getChildWithName (schema::TRACKS).getChild (0);
    CHECK_FALSE (track.hasProperty (schema::channelVolume));        // no finite number: its default, as a file's
    CHECK (track[schema::trackName].toString() == "1e12345678901"); // a name, as it was
    CHECK (track[schema::channelPan].toString() == "0");

    const auto slices = track.getChildWithName (schema::DEVICE).getChildWithName (schema::SOUNDS)[schema::samplerSlices].toString();
    CHECK_FALSE (slices.contains ("12345678901"));

    // And a piece joined alone, a track's, which no checks of a whole song see (makeExponentsReadable(), known bug 170): its
    // numbers made readable all the same, as JUCE's reader takes them.
    const auto trackPiece = juce::String (R"(<TRACK id="5" kind="track" name="1e12345678901" volume="1e12345678901"/>)");
    const auto alone = phazite::joinSong ("track", [&trackPiece] (const juce::String&) -> std::optional<juce::MemoryBlock>
                                          { return juce::MemoryBlock (trackPiece.toRawUTF8(), trackPiece.getNumBytesAsUTF8()); });
    REQUIRE (alone.isValid());
    CHECK (alone[schema::channelVolume].toString() == "1e999");
    CHECK (alone[schema::trackName].toString() == "1e12345678901");
    CHECK ((double) track.getChildWithName (schema::CLIP)[schema::clipLength] > 0.0); // brought within its range
}

TEST_CASE ("A send to a return the song hasn't is left out, with what modulated it")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* lead = Edited::findTrack (song, "Lead");
            auto* send = lead->createNewChildElement ("SEND");
            send->setAttribute ("id", "777770");
            send->setAttribute ("to", "777771");
            auto* modulator = lead->createNewChildElement ("MODULATOR");
            modulator->setAttribute ("id", "777772");
            modulator->setAttribute ("type", "lfo");
            auto* modulates = modulator->createNewChildElement ("MODULATES");
            modulates->setAttribute ("device", "777770");
            modulates->setAttribute ("parameter", "send level");
            modulates->setAttribute ("amount", 0.5);
        });
    REQUIRE (project != nullptr);
    const auto state = project->getSong().getState();
    CHECK (findAll (state, schema::SEND).empty());
    CHECK (findAll (state, schema::MODULATES).empty());
    CHECK (findAll (state, schema::MODULATOR).size() == 1);
}

TEST_CASE ("A track preset's volume beyond any fader's loads at the top of its range, and its sounds that aren't audio files stay out")
{
    TestSong s;
    TemporaryFolder temporary;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Keys");
    s.setInstrument (track, phazite::devices::DrumSampler::typeID);
    auto preset = phazite::makeTrackPreset (song, track);
    const auto saved = preset["track"];
    REQUIRE (saved.isObject());
    saved.getDynamicObject()->setProperty ("volume", 150.0);

    // A pad that names a file that isn't audio (copied into the project, it would go wherever the project goes), and one
    // on the network (which Windows would connect to as it looks).
    const auto secret = temporary.folder.getChildFile ("secret.txt");
    REQUIRE (secret.replaceWithText ("not a sound"));
    auto sounds = juce::var (new juce::DynamicObject());
    sounds.getDynamicObject()->setProperty ("pad36SoundFile", secret.getFullPathName());
    sounds.getDynamicObject()->setProperty ("pad38SoundFile", "\\\\fuzz.invalid\\share\\Snare.wav");
    REQUIRE (saved["instrument"].isObject());
    saved["instrument"].getDynamicObject()->setProperty ("sounds", sounds);

    const auto file = temporary.folder.getChildFile ("Loud.phazitepreset");
    REQUIRE (phazite::writePreset (preset, file));
    REQUIRE (phazite::loadPreset (s.context, file, track));

    const auto added = phazite::getTrackIDs (song).back();
    CHECK (phazite::getChannelVolume (song, added) == doctest::Approx (phazite::highestVolume));
    CHECK (s.getProject().getAudioFolder().findChildFiles (juce::File::findFiles, true, "*.txt").isEmpty());
    checkSong (song);
}

TEST_CASE ("A DAWproject whose files share their compressed bytes unpacks no more than an archive of its size can hold")
{
    TemporaryFolder temporary;

    // Phazite's own project.xml of a song with an audio clip, its clip there ten times, each of a file of its own name.
    juce::String project;
    juce::StringArray names;

    {
        TestSong s;
        const auto drums = s.addTrack ("Drums");
        s.addAudioClip (drums, writeSound (s.getSoundsFolder().getChildFile ("Loop.wav"), 0.25), 0.0);
        auto content = phazite::describeDawProject (*phazite::findSong (s.context), "Sharing", {});
        content.bounces.clear();
        juce::XmlElement* clip = nullptr;
        const std::function<void (juce::XmlElement&)> find = [&] (juce::XmlElement& element)
        {
            for (auto* child : element.getChildIterator())
            {
                if (child->hasTagName ("Clip") && child->getChildByName ("Audio") != nullptr)
                    clip = child;

                find (*child);
            }
        };

        find (*content.project);
        REQUIRE (clip != nullptr);
        auto* clips = content.project->findParentElementOf (clip);

        for (int i = 0; i < 10; ++i)
        {
            names.add ("audio/Zeros" + juce::String (i) + ".wav");
            auto* copy = i == 0 ? clip : new juce::XmlElement (*clip);
            copy->getChildByName ("Audio")->getChildByName ("File")->setAttribute ("path", names[i]);

            if (i > 0)
                clips->addChildElement (copy);
        }

        project = content.project->toString();
    }

    // The zip by hand: project.xml stored, then one entry of 16 MB of zeros, deflated, whose bytes every file's entry names.
    constexpr juce::int64 sharedSize = 16 << 20;
    constexpr int dosDate = ((2026 - 1980) << 25) | (1 << 21) | (1 << 16); // 1 January 2026, at midnight
    juce::MemoryOutputStream zip, central;
    int entries = 0;

    const auto writeName = [] (juce::OutputStream& out, const juce::String& name)
    { out.write (name.toRawUTF8(), name.getNumBytesAsUTF8()); };

    const auto writeLocal = [&] (const juce::String& name, const void* data, size_t size, int method, juce::int64 uncompressed)
    {
        const auto offset = (int) zip.getPosition();
        zip.writeInt (0x04034b50);
        zip.writeShort (20);
        zip.writeShort (0);
        zip.writeShort ((short) method);
        zip.writeInt (dosDate);
        zip.writeInt (0); // the checksum, which no reader here checks
        zip.writeInt ((int) size);
        zip.writeInt ((int) uncompressed);
        zip.writeShort ((short) name.getNumBytesAsUTF8());
        zip.writeShort (0);
        writeName (zip, name);
        zip.write (data, size);
        return offset;
    };

    const auto writeCentral = [&] (const juce::String& name, int offset, int method, juce::int64 compressed, juce::int64 uncompressed)
    {
        central.writeInt (0x02014b50);
        central.writeShort (20);
        central.writeShort (20);
        central.writeShort (0);
        central.writeShort ((short) method);
        central.writeInt (dosDate);
        central.writeInt (0);
        central.writeInt ((int) compressed);
        central.writeInt ((int) uncompressed);
        central.writeShort ((short) name.getNumBytesAsUTF8());
        central.writeShort (0);
        central.writeShort (0);
        central.writeShort (0);
        central.writeShort (0);
        central.writeInt (0);
        central.writeInt (offset);
        writeName (central, name);
        ++entries;
    };

    const auto text = project.toStdString();
    writeCentral ("project.xml", writeLocal ("project.xml", text.data(), text.size(), 0, (juce::int64) text.size()), 0,
                  (juce::int64) text.size(), (juce::int64) text.size());

    juce::MemoryOutputStream deflated;

    {
        juce::GZIPCompressorOutputStream compressor (deflated, 9, juce::GZIPCompressorOutputStream::windowBitsRaw);
        const juce::HeapBlock<char> zeros ((size_t) (1 << 20), true);

        for (juce::int64 written = 0; written < sharedSize; written += 1 << 20)
            compressor.write (zeros, (size_t) (1 << 20));
    }

    const auto shared = writeLocal (names[0], deflated.getData(), deflated.getDataSize(), 8, sharedSize);

    for (const auto& name : names)
        writeCentral (name, shared, 8, (juce::int64) deflated.getDataSize(), sharedSize);

    const auto centralStart = (int) zip.getPosition();
    zip.write (central.getData(), central.getDataSize());
    zip.writeInt (0x06054b50);
    zip.writeShort (0);
    zip.writeShort (0);
    zip.writeShort ((short) entries);
    zip.writeShort ((short) entries);
    zip.writeInt ((int) central.getDataSize());
    zip.writeInt (centralStart);
    zip.writeShort (0);
    const auto file = temporary.folder.getChildFile ("Sharing.dawproject");
    REQUIRE (file.replaceWithData (zip.getData(), zip.getDataSize()));
    REQUIRE (file.getSize() * 1100 < 10 * sharedSize); // what it would unpack to is far past what it can hold

    // Taken into a new song: the files come out as far as the archive can hold them, and no further.
    const auto read = phazite::readDawProject (file);
    REQUIRE (read.project != nullptr);
    const auto into = temporary.folder.getChildFile ("Imported").getChildFile ("Imported.phazite");
    REQUIRE (into.getParentDirectory().createDirectory());
    auto song = phazite::createSong (into);
    const auto said = phazite::takeDawProject (*song, read);
    juce::int64 unpacked = 0;

    for (const auto& audio : into.getSiblingFile ("Audio").findChildFiles (juce::File::findFiles, true))
    {
        CHECK (audio.getSize() == sharedSize); // whole, and none left empty where one was refused
        unpacked += audio.getSize();
    }

    CHECK (unpacked <= phazite::getUnpackLimit (file));
    CHECK (unpacked >= 3 * sharedSize); // the first files, whole, until the limit
    CHECK (said.joinIntoString ("\n").contains ("audio clips whose files aren't in it"));
    checkSong (*song);
}

//==============================================================================
// Projects Phazite saved itself open as they were saved: makeConsistent() repairs only what no Phazite writes.

namespace
{
// A track's audio clip, as the song keeps it (its file by its path: Schema.h, clipFile); one of no clip's, without a file,
// for a track without one.
phazite::ClipValue findAudioClip (const phazite::Song& song, phazite::TrackID track)
{
    for (const auto& clip : phazite::getClipValues (song, track))
        if (clip.isAudio())
            return clip;

    return {};
}

// A track's audio clip given another path for its file, in the song's history.
void setAudioClipFile (phazite::Song& song, phazite::TrackID track, const juce::String& path)
{
    auto clip = findAudioClip (song, track);
    clip.sound.file = path;
    phazite::setClip (song, clip);
}
} // namespace

TEST_CASE ("A sound file's name with a ':' in it, which Linux allows, opens as it was saved")
{
    TestSong s;
    const auto track = s.addTrack ("Loop");
    s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Loop.wav"), 0.25), 0.0);
    setAudioClipFile (*phazite::findSong (s.context), track, "Audio/Loop 1:2.wav");
    s.reopen();
    CHECK (findAudioClip (*phazite::findSong (s.context), track).sound.file == "Audio/Loop 1:2.wav");
}

TEST_CASE ("A full path Phazite keeps where it couldn't copy a sound in opens as it was saved, its name whatever it is")
{
    // As copyIntoProject() leaves an audio clip's file when the copy fails, and as Swap saved with Save a Copy leaves a pad's.
    TestSong s;
    const auto track = s.addTrack ("Loop");
    s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Loop.wav"), 0.25), 0.0);
    const auto outside = writeSound (s.getSoundsFolder().getChildFile ("Kick #1, @home; take.wav"), 0.25).getFullPathName();
    setAudioClipFile (*phazite::findSong (s.context), track, outside);
    s.reopen();
    auto& song = *phazite::findSong (s.context);
    const auto clip = findAudioClip (song, track);
    CHECK (clip.sound.file == outside);
    CHECK (phazite::findProjectFile (song, clip.sound.file).existsAsFile());
}

TEST_CASE ("A file on a network share is looked for in the Audio folder by its own name, whatever it holds")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            Edited::findTrack (song, "Audio")
                ->getChildByName ("CLIP")
                ->setAttribute ("file", "\\\\fuzz.invalid\\share\\Kick #1, @home; take.wav");
        });
    REQUIRE (project != nullptr);
    CHECK (findAudioClip (project->getSong(), edited.audio).sound.file == "Audio/Kick #1, @home; take.wav");
}

TEST_CASE ("Notes a tempo change made shorter than the shortest a note is drawn open as long as they were saved")
{
    // With Patterns Follow Tempo off, a slower tempo leaves the notes at their times: a 64th note becomes a 128th.
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Hats");
    const auto clip = s.addMidiClip (track, 0.0, 4.0);
    s.addNote (clip, 42, 0.0, 1.0 / 64.0);
    phazite::togglePatternsFollowTempo (song);
    REQUIRE_FALSE (phazite::doPatternsFollowTempo (song));
    s.setTempo (60.0);
    const auto note = phazite::copyWholeClip (song, clip).getChildWithName (schema::NOTE);
    const auto length = (double) note[schema::noteLength];
    REQUIRE (length < 1.0 / 64.0);

    s.reopen();
    const auto& reopenedSong = *phazite::findSong (s.context);
    const auto reopened = phazite::copyWholeClip (reopenedSong, clip).getChildWithName (schema::NOTE);
    CHECK ((double) reopened[schema::noteLength] == doctest::Approx (length));
}

TEST_CASE ("A chain's crossover's automation stays while its container isn't split into bands")
{
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Bus");
    const auto container = phazite::addContainer (song, track);
    REQUIRE (container.isValid());
    const auto second = phazite::addChain (song, container);
    phazite::setContainerSplit (song, container, phazite::ContainerSplit::bands);
    const phazite::ParameterRef crossover { second, schema::crossoverParameter };
    s.addAutomation (crossover, { { at (0.0), 500.0f }, { at (4.0), 2000.0f } });
    phazite::setContainerSplit (song, container, phazite::ContainerSplit::layers);
    REQUIRE (phazite::getPoints (song, crossover).size() == 2);

    s.reopen();
    CHECK (phazite::getPoints (*phazite::findSong (s.context), crossover).size() == 2);
}

TEST_CASE ("Elements and properties Phazite doesn't know stay where they are, as they are, and are saved again, but in a clip's notes")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* future = song.createNewChildElement ("FUTURE");
            future->setAttribute ("kept", "yes");
            future->createNewChildElement ("TRACK")->setAttribute ("id", Edited::findTrack (song, "Lead")->getStringAttribute ("id"));
            future->createNewChildElement ("NOTE")->setAttribute ("pitch", "500");
            auto* lead = Edited::findTrack (song, "Lead");
            lead->setAttribute ("laterProperty", "1");
            lead->createNewChildElement ("TOOLDATA")->setAttribute ("by", "another tool");
            lead->getChildByName ("CLIP")->getChildByName ("NOTE")->createNewChildElement ("EXPRESSION")->setAttribute ("x", "2");
        });
    REQUIRE (project != nullptr);
    REQUIRE (project->save().wasOk());
    const auto saved = juce::parseXML (project->getFile());
    REQUIRE (saved != nullptr);

    const auto* future = saved->getChildByName ("FUTURE");
    REQUIRE (future != nullptr);
    CHECK (future->getStringAttribute ("kept") == "yes");
    CHECK (future->getNumChildElements() == 2);
    CHECK (future->getChildByName ("NOTE")->getStringAttribute ("pitch") == "500");
    const auto* lead = Edited::findTrack (*saved, "Lead");
    CHECK (lead->getStringAttribute ("laterProperty") == "1");
    CHECK (lead->getChildByName ("TOOLDATA") != nullptr);
    // But a note's and a controller's value: what Phazite doesn't know on a NOTE or a CONTROL isn't kept, as a clip's notes
    // are values beside the song's tree (docs/project-format.md).
    CHECK (lead->getChildByName ("CLIP")->getChildByName ("NOTE")->getChildByName ("EXPRESSION") == nullptr);
}

TEST_CASE ("A DAWproject's project.xml with a byte order mark and no XML declaration is read")
{
    TemporaryFolder temporary;
    juce::String text;

    {
        TestSong s;
        const auto lead = s.addTrack ("Lead");
        s.addNote (s.addMidiClip (lead, 0.0, 4.0), 60, 0.0, 1.0);
        auto content = phazite::describeDawProject (*phazite::findSong (s.context), "Marked", {});
        content.bounces.clear();
        text = content.project->toString (juce::XmlElement::TextFormat().withoutHeader());
    }

    const auto file = temporary.folder.getChildFile ("Marked.dawproject");

    {
        juce::MemoryOutputStream xml;
        xml.write ("\xef\xbb\xbf", 3); // UTF-8's byte order mark
        xml << text;
        juce::ZipFile::Builder builder;
        builder.addEntry (std::make_unique<juce::MemoryInputStream> (xml.getMemoryBlock(), true), 0, "project.xml",
                          juce::Time (2026, 0, 1, 0, 0));
        juce::FileOutputStream out (file);
        REQUIRE (builder.writeToStream (out, nullptr));
    }

    const auto read = phazite::readDawProject (file);
    CHECK (read.project != nullptr);
}

TEST_CASE ("A MIDI link to what the song hasn't, a controller's or a parameter's second link, and a controller out of range go as the "
           "song opens")
{
    Edited edited;

    // Two links, a device's parameter's and a fader's, saved.
    {
        auto& song = *phazite::findSong (edited.s.context);
        const phazite::UndoStep step (song, "Link MIDI Controllers");
        phazite::linkMidiController (song, { 1, 21 }, { edited.synth, "cutoff" });
        phazite::linkMidiController (song, { 2, 7 }, edited.s.getVolumeParameter (edited.lead));
    }

    REQUIRE (edited.s.getProject().save().wasOk());
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* links = song.getChildByName ("MIDILINKS");
            REQUIRE (links != nullptr);
            REQUIRE (links->getNumChildElements() == 2);
            const auto* first = links->getChildElement (0);
            auto* second = links->getChildElement (1);

            auto* gone = new juce::XmlElement (*first); // a link of a device the song hasn't
            gone->setAttribute ("device", "777777");
            gone->setAttribute ("number", "50");

            auto* sameController = new juce::XmlElement (*second); // the first's controller, another parameter
            sameController->setAttribute ("channel", first->getStringAttribute ("channel"));
            sameController->setAttribute ("number", first->getStringAttribute ("number"));

            auto* sameParameter = new juce::XmlElement (*first); // the first's parameter, another controller
            sameParameter->setAttribute ("number", "60");

            for (auto* added : { gone, sameController, sameParameter })
                links->addChildElement (added);

            // No controller a link takes: brought within range, it would be another controller (16 and 119).
            second->setAttribute ("channel", "40");
            second->setAttribute ("number", "125");
        });
    REQUIRE (project != nullptr);
    const auto links = phazite::getMidiLinksValue (project->getSong()).links;
    REQUIRE (links.size() == 1);
    CHECK (links[0].channel == 1);
    CHECK (links[0].number == 21);
    CHECK (links[0].parameter == "cutoff");
}

TEST_CASE ("Damaged MIDI links open as a whole value, each link as its description reads it, saved and opened again the same file")
{
    // The song takes in its file's links as it opens (Song::holdValues(): MidiLinksValue.h, readHeldMidiLinks()): numbers
    // written as text, or not whole, a device's ID as a number, what Phazite doesn't know on the MIDILINKS and a link, a child
    // of another type, a second link of one parameter (the first stays), and an empty MIDILINKS. Each opens holding together
    // (the check, in Edited::open()), holds what its description reads, in the file's order, and is the same once saved and
    // opened again, its MIDILINKS written without what Phazite doesn't know, and the same file saved twice. An empty MIDILINKS
    // stays one.
    struct Case
    {
        const char* about;
        const char* links; // "SYNTH" stands for the synth's ID
        std::vector<juce::String> expected;
    };

    const auto describe = [] (const phazite::Song& song)
    {
        std::vector<juce::String> lines;

        if (! song.getState().getChildWithName (schema::MIDILINKS).isValid())
            return lines;

        lines.push_back ("links");

        for (const auto& link : phazite::getMidiLinksValue (song).links)
            lines.push_back (juce::String (link.channel) + " " + juce::String (link.number) + " " + link.parameter);

        return lines;
    };

    const Case cases[] = {
        { "numbers as text, and not whole",
          R"(<MIDILINKS><MIDILINK channel=" 2 " number="7.4" device="SYNTH" parameter="cutoff"/></MIDILINKS>)",
          { "links", "2 7 cutoff" } },
        { "what Phazite doesn't know",
          R"(<MIDILINKS colour="red"><LINE/><MIDILINK channel="1" number="21" device="SYNTH" parameter="drive" swing="0.2"/>)"
          R"(</MIDILINKS>)",
          { "links", "1 21 drive" } },
        { "a second link of one parameter, after another of another, in the file's order",
          R"(<MIDILINKS><MIDILINK channel="3" number="3" device="SYNTH" parameter="resonance"/>)"
          R"(<MIDILINK channel="1" number="1" device="SYNTH" parameter="cutoff"/>)"
          R"(<MIDILINK channel="1" number="2" device="SYNTH" parameter="resonance"/></MIDILINKS>)",
          { "links", "3 3 resonance", "1 1 cutoff" } },
        { "an empty MIDILINKS", R"(<MIDILINKS/>)", { "links" } },
    };

    for (const auto& each : cases)
    {
        INFO (juce::String (each.about));
        Edited edited;
        const auto synth = juce::String ((juce::int64) edited.synth.getRaw());
        const auto project = edited.open (
            [&each, &synth] (juce::XmlElement& song)
            {
                auto links = juce::parseXML (juce::String (each.links).replace ("SYNTH", synth));
                REQUIRE (links != nullptr);
                song.addChildElement (links.release());
            });
        REQUIRE (project != nullptr);
        CHECK (describe (project->getSong()) == each.expected);

        const auto links = phazite::test::copyWholeSong (project->getSong()).getChildWithName (schema::MIDILINKS);
        REQUIRE (links.isValid());
        CHECK (links.getNumProperties() == 0);

        for (const auto& child : links)
        {
            CHECK (child.hasType (schema::MIDILINK));
            CHECK (child.getNumProperties() == 4);
        }

        REQUIRE (project->save().wasOk());
        const auto firstSave = edited.file.loadFileAsString();
        juce::String error;
        const auto again = phazite::Project::open (edited.file, false, error);
        REQUIRE_MESSAGE (again != nullptr, error.toStdString());
        checkSong (again->getSong());
        CHECK (describe (again->getSong()) == each.expected);
        CHECK (phazite::test::copyWholeSong (again->getSong()).getChildWithName (schema::MIDILINKS).isEquivalentTo (links));
        REQUIRE (again->save().wasOk());
        CHECK (edited.file.loadFileAsString() == firstSave);
    }
}

namespace
{
// An alias group of three MIDI clips on the Lead's track, written into a project's file: the Lead's clip (800001, from beat 0)
// given notes 900001 and 900002 and a mod wheel's value besides the note it holds, and two copies of it (800002 at beat 8 and
// 800003 at beat 16), all three in the group 800010. Returns the three, in the song's order.
std::vector<juce::XmlElement*> writeGroupOfThree (juce::XmlElement& song)
{
    auto* lead = Edited::findTrack (song, "Lead");
    auto* clip = lead->getChildByName ("CLIP");
    REQUIRE (clip != nullptr);
    clip->setAttribute ("id", "800001");
    clip->setAttribute ("link", "800010");

    for (const auto& [id, pitch, start] : { std::tuple { "900001", "64", "1" }, std::tuple { "900002", "67", "2" } })
    {
        auto* note = clip->createNewChildElement ("NOTE");
        note->setAttribute ("id", id);
        note->setAttribute ("pitch", pitch);
        note->setAttribute ("start", start);
        note->setAttribute ("length", "0.5");
        note->setAttribute ("velocity", "90");
    }

    auto* control = clip->createNewChildElement ("CONTROL");
    control->setAttribute ("kind", "controller");
    control->setAttribute ("number", "1");
    control->setAttribute ("beat", "0.5");
    control->setAttribute ("value", "40");

    std::vector<juce::XmlElement*> clips { clip };

    for (const auto& [id, start] : { std::pair { "800002", "8" }, std::pair { "800003", "16" } })
    {
        auto* copy = new juce::XmlElement (*clip);
        copy->setAttribute ("id", id);
        copy->setAttribute ("start", start);
        lead->addChildElement (copy);
        clips.push_back (copy);
    }

    return clips;
}

// A clip's NOTE and CONTROL elements, in order.
std::vector<juce::XmlElement*> getEvents (juce::XmlElement& clip)
{
    std::vector<juce::XmlElement*> events;

    for (auto* child : clip.getChildIterator())
        if (child->hasTagName ("NOTE") || child->hasTagName ("CONTROL"))
            events.push_back (child);

    return events;
}

// An opened song's clip by its ID, its alias group (empty for none), and its notes as the model reads them.
// A clip of the song, whole (model/base/internal/WholeState.h): its notes as a save writes them.
juce::ValueTree findClipByID (const phazite::Song& song, std::uint64_t id)
{
    return phazite::copyWholeClip (song, phazite::ClipID::fromRaw (id));
}

juce::String getLink (const phazite::Song& song, std::uint64_t id)
{
    const auto clip = findClipByID (song, id);
    REQUIRE (clip.isValid());
    return clip[schema::clipLink].toString();
}

juce::String describeNotesOf (const phazite::Song& song, std::uint64_t id)
{
    return describeClipNotes (song, phazite::ClipID::fromRaw (id));
}
} // namespace

TEST_CASE ("A clip alone in its alias group opens without its link")
{
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            auto* clip = Edited::findTrack (song, "Lead")->getChildByName ("CLIP");
            REQUIRE (clip != nullptr);
            clip->setAttribute ("id", "800001");
            clip->setAttribute ("link", "800010");
        });
    REQUIRE (project != nullptr);
    CHECK (getLink (project->getSong(), 800001).isEmpty());
    CHECK (phazite::getClipNotes (project->getSong(), phazite::ClipID::fromRaw (800001)).size() == 1);
}

TEST_CASE ("A clip of an alias group whose notes or values differ from the group's first clip's opens as a clip of its own, its "
           "notes kept")
{
    // Three clips of a group; the one that differs, by a note's value, a note more, the notes' order or a controller's value,
    // made a clip of its own, holding what it held; the other two still a group, under its link. With the first clip the one
    // that differs, the other two, which hold the same as each other, each differ from it: all three of their own.
    struct Difference
    {
        const char* what;
        std::function<void (juce::XmlElement& clip)> make;
        size_t notes; // the clip that differs holds as many
    };

    const std::vector<Difference> differences {
        { "a note's value", [] (juce::XmlElement& clip) { getEvents (clip)[1]->setAttribute ("velocity", "91"); }, 3 },
        { "a note more",
          [] (juce::XmlElement& clip)
          {
              auto* note = new juce::XmlElement (*getEvents (clip)[1]);
              note->setAttribute ("id", "900003");
              clip.addChildElement (note);
          },
          4 },
        { "the notes' order",
          [] (juce::XmlElement& clip)
          {
              auto* first = getEvents (clip)[0];
              clip.removeChildElement (first, false);
              clip.addChildElement (first);
          },
          3 },
        { "a controller's value", [] (juce::XmlElement& clip) { getEvents (clip)[3]->setAttribute ("value", "41"); }, 3 },
    };

    for (const auto& difference : differences)
    {
        for (const auto differing : { 1, 0 })
        {
            INFO (difference.what << ", in the group's " << (differing == 0 ? "first" : "second") << " clip");
            Edited edited;
            const auto project = edited.open (
                [&] (juce::XmlElement& song)
                {
                    auto clips = writeGroupOfThree (song);
                    REQUIRE (getEvents (*clips[0]).size() == 4);
                    difference.make (*clips[(size_t) differing]);
                });
            REQUIRE (project != nullptr);
            const auto& song = project->getSong();
            const std::uint64_t odd = differing == 0 ? 800001 : 800002;
            CHECK (getLink (song, odd).isEmpty());

            if (differing == 0)
            {
                CHECK (getLink (song, 800002).isEmpty());
                CHECK (getLink (song, 800003).isEmpty());
            }
            else
            {
                CHECK (getLink (song, 800001) == "800010");
                CHECK (getLink (song, 800003) == "800010");
                CHECK (describeNotesOf (song, 800001) == describeNotesOf (song, 800003));
            }

            CHECK (phazite::getClipNotes (song, phazite::ClipID::fromRaw (odd)).size() == difference.notes); // its notes kept
        }
    }
}

TEST_CASE ("An audio clip and MIDI clips under one link open with the audio clip on its own")
{
    // The Lead's two MIDI clips of a group, and the Audio track's clip, after them in the song's order, under the same link:
    // the audio clip loses it, the MIDI clips stay a group. And an audio clip and one MIDI clip: neither keeps it.
    for (const auto midiClips : { 2, 1 })
    {
        INFO (midiClips << " MIDI clips");
        Edited edited;
        std::uint64_t audioID = 0;
        const auto project = edited.open (
            [&] (juce::XmlElement& song)
            {
                auto clips = writeGroupOfThree (song);

                for (auto i = (int) clips.size(); --i >= midiClips;)
                    Edited::findTrack (song, "Lead")->removeChildElement (clips[(size_t) i], true);

                auto* audio = Edited::findTrack (song, "Audio")->getChildByName ("CLIP");
                REQUIRE (audio != nullptr);
                audio->setAttribute ("link", "800010");
                audioID = (std::uint64_t) audio->getStringAttribute ("id").getLargeIntValue();
            });
        REQUIRE (project != nullptr);
        const auto& song = project->getSong();
        CHECK (getLink (song, audioID).isEmpty());
        CHECK (getLink (song, 800001) == (midiClips == 2 ? "800010" : ""));

        if (midiClips == 2)
            CHECK (getLink (song, 800002) == "800010");
    }
}

TEST_CASE ("An alias group that holds together opens as it was written, with its IDs, and an edit through one clip reaches all")
{
    Edited edited;
    const auto project = edited.open ([] (juce::XmlElement& song) { writeGroupOfThree (song); });
    REQUIRE (project != nullptr);
    auto& song = project->getSong();

    for (const auto id : { std::uint64_t { 800001 }, std::uint64_t { 800002 }, std::uint64_t { 800003 } })
    {
        INFO ("clip " << id);
        CHECK (getLink (song, id) == "800010");
        CHECK (describeNotesOf (song, id) == describeNotesOf (song, 800001));
        std::vector<juce::String> ids;

        for (const auto& child : findClipByID (song, id))
            if (child.hasType (schema::NOTE))
                ids.push_back (child[schema::noteID].toString());

        REQUIRE (ids.size() == 3);
        CHECK (ids[1] == "900001");
        CHECK (ids[2] == "900002");
    }

    const auto notes = phazite::getClipNotes (song, phazite::ClipID::fromRaw (800003));
    REQUIRE (notes.size() == 3);
    phazite::setNoteVelocities (song, { notes[1].getRef() }, 30);
    CHECK (describeNotesOf (song, 800001) == describeNotesOf (song, 800003));
    CHECK (phazite::getNoteData (song, phazite::NoteRef { phazite::ClipID::fromRaw (800002), notes[1].id })->velocity == 30);
    checkSong (song);
}

TEST_CASE ("An alias group whose notes' IDs are given anew as it opens stays a group, each note given the same new ID in every "
           "clip")
{
    // Three clips of a group, the same in the file, whose notes' IDs the loader gives anew in each clip (Project.cpp: keepIDs()):
    // a note's given the same new ID in each, by its place and old ID, so that the group holds together. With one clip whose
    // note's pitch differs too: that one alone made a clip of its own.
    struct Wrong
    {
        const char* what;
        std::function<void (juce::XmlElement& clip)> make;
    };

    const std::vector<Wrong> wrongs {
        { "an ID of 0", [] (juce::XmlElement& clip) { getEvents (clip)[1]->setAttribute ("id", "0"); } },
        { "an ID at maxID",
          [] (juce::XmlElement& clip) { getEvents (clip)[1]->setAttribute ("id", juce::String ((juce::uint64) schema::maxID)); } },
        { "an ID twice in the clip", [] (juce::XmlElement& clip) { getEvents (clip)[2]->setAttribute ("id", "900001"); } },
    };

    for (const auto& wrong : wrongs)
    {
        for (const auto oneDiffers : { false, true })
        {
            INFO (wrong.what << (oneDiffers ? ", the second clip's first note's pitch differing" : ""));
            Edited edited;
            const auto project = edited.open (
                [&] (juce::XmlElement& song)
                {
                    const auto clips = writeGroupOfThree (song);

                    for (auto* clip : clips)
                        wrong.make (*clip);

                    if (oneDiffers)
                        getEvents (*clips[1])[0]->setAttribute ("pitch", "61");
                });
            REQUIRE (project != nullptr);
            const auto& song = project->getSong();

            // Each clip's notes' IDs, in order: each one of the song's own.
            const auto getNoteIDs = [&song] (std::uint64_t clip)
            {
                std::vector<std::uint64_t> ids;
                std::set<std::uint64_t> seen;

                for (const auto& child : findClipByID (song, clip))
                {
                    if (! child.hasType (schema::NOTE))
                        continue;

                    const auto id = phazite::readID (child[schema::noteID]);
                    CHECK (id != 0);
                    CHECK (id < schema::maxID);
                    CHECK (seen.insert (id).second);
                    ids.push_back (id);
                }

                CHECK (ids.size() == 3);
                return ids;
            };

            const auto first = getNoteIDs (800001);
            CHECK (getLink (song, 800001) == "800010");
            CHECK (getLink (song, 800003) == "800010");
            CHECK (getNoteIDs (800003) == first);
            CHECK (describeNotesOf (song, 800003) == describeNotesOf (song, 800001));

            if (oneDiffers)
            {
                CHECK (getLink (song, 800002).isEmpty());
                getNoteIDs (800002);
            }
            else
            {
                CHECK (getLink (song, 800002) == "800010");
                CHECK (getNoteIDs (800002) == first);
                CHECK (describeNotesOf (song, 800002) == describeNotesOf (song, 800001));
            }

            checkSong (song);
        }
    }
}

namespace
{
// An alias group of three audio clips on the Audio track, written into a project's file: the track's clip (800101, from beat
// 4) following the tempo, played at 120 BPM, pitched 2 semitones up and with two warp markers, and two copies of it (800102
// at beat 8 and 800103 at beat 16), all three in the group 800110. Returns the three, in the song's order.
std::vector<juce::XmlElement*> writeAudioGroupOfThree (juce::XmlElement& song)
{
    auto* audio = Edited::findTrack (song, "Audio");
    auto* clip = audio->getChildByName ("CLIP");
    REQUIRE (clip != nullptr);
    clip->setAttribute ("id", "800101");
    clip->setAttribute ("link", "800110");
    clip->setAttribute ("tempo", "120");
    clip->setAttribute ("pitch", "2");

    for (const auto& [time, playsAt] : { std::pair { "0", "0" }, std::pair { "0.125", "0.15" } })
    {
        auto* marker = clip->createNewChildElement ("WARP");
        marker->setAttribute ("time", time);
        marker->setAttribute ("playsAt", playsAt);
    }

    std::vector<juce::XmlElement*> clips { clip };

    for (const auto& [id, start] : { std::pair { "800102", "8" }, std::pair { "800103", "16" } })
    {
        auto* copy = new juce::XmlElement (*clip);
        copy->setAttribute ("id", id);
        copy->setAttribute ("start", start);
        audio->addChildElement (copy);
        clips.push_back (copy);
    }

    return clips;
}

// A clip's WARP elements, in order.
std::vector<juce::XmlElement*> getMarkers (juce::XmlElement& clip)
{
    std::vector<juce::XmlElement*> markers;

    for (auto* child : clip.getChildIterator())
        if (child->hasTagName ("WARP"))
            markers.push_back (child);

    return markers;
}

// What an opened song's audio clip shares with the others of its group, as the model reads it: its tempo, pitch and
// direction, and its warp markers in their order, each a time of the file with where it plays straight.
juce::String describeSoundOf (const phazite::Song& song, std::uint64_t id)
{
    const auto clip = phazite::ClipID::fromRaw (id);
    juce::StringArray markers;

    for (const auto& child : findClipByID (song, id))
        if (child.hasType (schema::WARP))
            markers.add (juce::String ((double) child[schema::warpTime], 3) + ":" + juce::String ((double) child[schema::warpPlaysAt], 3));

    return "tempo " + juce::String (phazite::getClipTempo (song, clip), 1) + ", pitch "
           + juce::String ((double) phazite::getClipPitch (song, clip), 1)
           + (phazite::isClipReversed (song, clip) ? ", backwards" : ", forwards") + ", markers " + markers.joinIntoString (" ");
}
} // namespace

TEST_CASE ("An audio clip of an alias group whose tempo, pitch, direction or warp markers differ from the group's first clip's "
           "opens as a clip of its own, playing as it was written")
{
    // Three audio clips of a group; the one that differs, by each value the group's audio clips share in turn and by its
    // warp markers, made a clip of its own, playing as it was written; the other two still a group, under its link. With the
    // first clip the one that differs, the other two each differ from it: all three of their own.
    struct Difference
    {
        const char* what;
        std::function<void (juce::XmlElement& clip)> make;
        const char* sound; // what the clip that differs plays
    };

    const std::vector<Difference> differences {
        { "its tempo", [] (juce::XmlElement& clip) { clip.setAttribute ("tempo", "100"); },
          "tempo 100.0, pitch 2.0, forwards, markers 0.000:0.000 0.125:0.150" },
        { "its pitch", [] (juce::XmlElement& clip) { clip.setAttribute ("pitch", "3"); },
          "tempo 120.0, pitch 3.0, forwards, markers 0.000:0.000 0.125:0.150" },
        { "its pitch, none of its own", [] (juce::XmlElement& clip) { clip.removeAttribute ("pitch"); },
          "tempo 120.0, pitch 0.0, forwards, markers 0.000:0.000 0.125:0.150" },
        { "its direction", [] (juce::XmlElement& clip) { clip.setAttribute ("reversed", "1"); },
          "tempo 120.0, pitch 2.0, backwards, markers 0.000:0.000 0.125:0.150" },
        { "a warp marker's place", [] (juce::XmlElement& clip) { getMarkers (clip)[1]->setAttribute ("playsAt", "0.2"); },
          "tempo 120.0, pitch 2.0, forwards, markers 0.000:0.000 0.125:0.200" },
        { "a warp marker more",
          [] (juce::XmlElement& clip)
          {
              auto* marker = clip.createNewChildElement ("WARP");
              marker->setAttribute ("time", "0.2");
              marker->setAttribute ("playsAt", "0.22");
          },
          "tempo 120.0, pitch 2.0, forwards, markers 0.000:0.000 0.125:0.150 0.200:0.220" },
        { "the warp markers' order",
          [] (juce::XmlElement& clip)
          {
              auto* first = getMarkers (clip)[0];
              clip.removeChildElement (first, false);
              clip.addChildElement (first);
          },
          "tempo 120.0, pitch 2.0, forwards, markers 0.125:0.150 0.000:0.000" },
    };

    for (const auto& difference : differences)
    {
        for (const auto differing : { 1, 0 })
        {
            INFO (difference.what << ", in the group's " << (differing == 0 ? "first" : "second") << " clip");
            Edited edited;
            const auto project = edited.open (
                [&] (juce::XmlElement& song)
                {
                    auto clips = writeAudioGroupOfThree (song);
                    REQUIRE (getMarkers (*clips[0]).size() == 2);
                    difference.make (*clips[(size_t) differing]);
                });
            REQUIRE (project != nullptr);
            const auto& song = project->getSong();
            const std::uint64_t odd = differing == 0 ? 800101 : 800102;
            CHECK (getLink (song, odd).isEmpty());

            if (differing == 0)
            {
                CHECK (getLink (song, 800102).isEmpty());
                CHECK (getLink (song, 800103).isEmpty());
            }
            else
            {
                CHECK (getLink (song, 800101) == "800110");
                CHECK (getLink (song, 800103) == "800110");
                CHECK (describeSoundOf (song, 800101) == describeSoundOf (song, 800103));
            }

            CHECK (describeSoundOf (song, odd) == difference.sound); // as it was written
            checkSong (song);
        }
    }
}

TEST_CASE ("An audio alias group that holds together opens as it was written, each clip's own values its own, and an edit through "
           "one clip reaches all")
{
    // The second clip's gain, fade and the part of the file it plays its own; the third's direction written as forwards, as
    // a clip without one plays: none differs from the first's in what they share.
    Edited edited;
    const auto project = edited.open (
        [] (juce::XmlElement& song)
        {
            const auto clips = writeAudioGroupOfThree (song);
            clips[1]->setAttribute ("gain", "-6");
            clips[1]->setAttribute ("fadeIn", "0.05");
            clips[1]->setAttribute ("fileStart", "0.05");
            clips[2]->setAttribute ("reversed", "0");
        });
    REQUIRE (project != nullptr);
    auto& song = project->getSong();

    for (const auto id : { std::uint64_t { 800101 }, std::uint64_t { 800102 }, std::uint64_t { 800103 } })
    {
        INFO ("clip " << id);
        CHECK (getLink (song, id) == "800110");
        CHECK (describeSoundOf (song, id) == "tempo 120.0, pitch 2.0, forwards, markers 0.000:0.000 0.125:0.150");
    }

    CHECK (phazite::getClipGain (song, phazite::ClipID::fromRaw (800102)) == doctest::Approx (-6.0f));
    CHECK (phazite::getClipGain (song, phazite::ClipID::fromRaw (800101)) == doctest::Approx (0.0f));

    {
        const phazite::UndoStep step (song, "Change Clip Pitch");
        phazite::setClipPitch (song, phazite::ClipID::fromRaw (800103), -5.0f);
    }

    for (const auto id : { std::uint64_t { 800101 }, std::uint64_t { 800102 } })
        CHECK (describeSoundOf (song, id) == describeSoundOf (song, 800103));

    CHECK (phazite::getClipPitch (song, phazite::ClipID::fromRaw (800101)) == doctest::Approx (-5.0f));
    checkSong (song);
}

TEST_CASE ("An audio clip of an alias group that plays another file than the group's first clip opens as a clip of its own, "
           "playing its file")
{
    // Three audio clips of a group, alike but for the file one of them names: another file of the project's Audio folder,
    // there to play. Warp markers are times in one file, so a clip of another file can't share them: it is made a clip of its
    // own, its file kept; the other two stay a group. With the first clip the one that names another, each other differs
    // from it: all three of their own.
    for (const auto differing : { 2, 0 })
    {
        INFO ("the group's " << (differing == 0 ? "first" : "third") << " clip names another file");
        Edited edited;
        const auto project = edited.open (
            [&] (juce::XmlElement& song)
            {
                auto clips = writeAudioGroupOfThree (song);
                writeSound (edited.s.getProject().getAudioFolder().getChildFile ("Other.wav"), 0.25);
                REQUIRE (clips[0]->getStringAttribute ("file") == "Audio/Loop.wav");
                clips[(size_t) differing]->setAttribute ("file", "Audio/Other.wav");
            });
        REQUIRE (project != nullptr);
        const auto& song = project->getSong();
        const std::uint64_t odd = differing == 0 ? 800101 : 800103;
        CHECK (getLink (song, odd).isEmpty());
        CHECK (findClipByID (song, odd)[schema::clipFile].toString() == "Audio/Other.wav");
        CHECK (phazite::getClipFile (song, phazite::ClipID::fromRaw (odd)).existsAsFile());
        CHECK (describeSoundOf (song, odd) == "tempo 120.0, pitch 2.0, forwards, markers 0.000:0.000 0.125:0.150");

        if (differing == 0)
        {
            CHECK (getLink (song, 800102).isEmpty());
            CHECK (getLink (song, 800103).isEmpty());
        }
        else
        {
            CHECK (getLink (song, 800101) == "800110");
            CHECK (getLink (song, 800102) == "800110");
            CHECK (findClipByID (song, 800102)[schema::clipFile].toString() == "Audio/Loop.wav");
        }

        checkSong (song);
    }
}
