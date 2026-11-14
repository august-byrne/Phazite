// SPDX-License-Identifier: AGPL-3.0-or-later
// Projects and presets that parse but don't hold together, as a file from someone else could be: each opens into a song
// that passes the consistency check (SongCheck.h), with what didn't hold left out or brought within its range
// (model/project/internal/Project.h: makeConsistent()), or is refused. And the limits on what a DAWproject unpacks.
#include "GoldenFiles.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <functional>

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
    CHECK (findAll (project->getSong().getState(), schema::CLIP).size() == 2); // the MIDI clip and the audio clip, each once
    CHECK (findAll (project->getSong().getState(), schema::NOTE).size() == 1);
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

    for (const auto& clip : findAll (project->getSong().getState(), schema::CLIP))
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
    const auto curves = findAll (project->getSong().getState(), schema::CURVE);
    REQUIRE (curves.size() == 1);
    CHECK ((float) curves.front().getChild (0)[schema::pointValue] == doctest::Approx (1.0f));
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
// A track's audio clip's file, as the song keeps it.
juce::ValueTree findAudioClip (phazite::Song& song, phazite::TrackID track)
{
    for (const auto clip : phazite::getClips (song, track))
        if (const auto state = phazite::findClipState (song, clip); state[schema::clipKind].toString() == schema::clipIsAudio)
            return state;

    return {};
}
} // namespace

TEST_CASE ("A sound file's name with a ':' in it, which Linux allows, opens as it was saved")
{
    TestSong s;
    const auto track = s.addTrack ("Loop");
    s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Loop.wav"), 0.25), 0.0);
    findAudioClip (*phazite::findSong (s.context), track).setProperty (schema::clipFile, "Audio/Loop 1:2.wav", nullptr);
    s.reopen();
    CHECK (findAudioClip (*phazite::findSong (s.context), track)[schema::clipFile].toString() == "Audio/Loop 1:2.wav");
}

TEST_CASE ("A full path Phazite keeps where it couldn't copy a sound in opens as it was saved, its name whatever it is")
{
    // As copyIntoProject() leaves an audio clip's file when the copy fails, and as Swap saved with Save a Copy leaves a pad's.
    TestSong s;
    const auto track = s.addTrack ("Loop");
    s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Loop.wav"), 0.25), 0.0);
    const auto outside = writeSound (s.getSoundsFolder().getChildFile ("Kick #1, @home; take.wav"), 0.25).getFullPathName();
    findAudioClip (*phazite::findSong (s.context), track).setProperty (schema::clipFile, outside, nullptr);
    s.reopen();
    auto& song = *phazite::findSong (s.context);
    const auto clip = findAudioClip (song, track);
    CHECK (clip[schema::clipFile].toString() == outside);
    CHECK (phazite::findProjectFile (song, clip[schema::clipFile].toString()).existsAsFile());
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
    CHECK (findAudioClip (project->getSong(), edited.audio)[schema::clipFile].toString() == "Audio/Kick #1, @home; take.wav");
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
    const auto note = phazite::findClipState (song, clip).getChildWithName (schema::NOTE);
    const auto length = (double) note[schema::noteLength];
    REQUIRE (length < 1.0 / 64.0);

    s.reopen();
    const auto reopened = phazite::findClipState (*phazite::findSong (s.context), clip).getChildWithName (schema::NOTE);
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

TEST_CASE ("Elements and properties Phazite doesn't know stay where they are, as they are, and are saved again")
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
    CHECK (lead->getChildByName ("CLIP")->getChildByName ("NOTE")->getChildByName ("EXPRESSION") != nullptr);
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
