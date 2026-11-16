// SPDX-License-Identifier: AGPL-3.0-or-later
// DAWproject (model/project/internal/DawProject.h; docs/design/versions.md: DAWproject with versions, steps D1 and D2): a song
// written as one, in DAWproject's names (Project.xsd, version 1.0), what another DAW can't play bounced beside it, and the
// zip, from the song as it is and from an old version.
#include "SongCheck.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "app/ProjectSession.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "services/RecentProjects.h"
#include "services/Settings.h"

#include <functional>
#include <map>
#include <tuple>

namespace
{
using namespace phazite::test;

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name)
{
    REQUIRE (waitForVersionHistory (context));
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (waitForVersionHistory (context));
    return phazite::getLastVersion (context)->id;
}

// The element of a tag whose attribute is a value, anywhere under an element; or nullptr.
const juce::XmlElement* find (const juce::XmlElement& in, const juce::String& tag, const juce::String& attribute, const juce::String& value)
{
    for (const auto* child : in.getChildIterator())
    {
        if (child->hasTagName (tag) && child->getStringAttribute (attribute) == value)
            return child;

        if (const auto* found = find (*child, tag, attribute, value))
            return found;
    }

    return nullptr;
}

juce::String id (const char* prefix, phazite::TrackID track)
{
    return prefix + juce::String ((juce::int64) track.getRaw());
}

// A song with what DAWproject has places for, and a track on Phazite's synth, which another DAW can't play.
struct Song
{
    TestSong s;
    phazite::TrackID bass = s.addTrack ("Bass"), lead = s.addTrack ("Lead");
    phazite::ClipID riff;
    phazite::TrackID echo;

    Song()
    {
        s.setTempo (128.0);
        s.setTimeSignature (3, 4);
        riff = s.addMidiClip (bass, 0.0, 4.0);
        s.addNote (riff, 36, 0.0, 1.0, 100);
        s.addNote (riff, 43, 2.0, 0.5, 127);
        s.setVolume (bass, -6.0f);
        s.setPan (bass, 0.5f);
        s.setInstrument (lead, phazite::devices::Synth::typeID);
        s.addNote (s.addMidiClip (lead, 4.0, 4.0), 60, 0.0, 2.0);
        echo = s.addReturn (bass, phazite::devices::Equalizer::typeID, -12.0f); // an effect of Phazite's own on it, too
        s.addAutomation (s.getVolumeParameter (bass),
                         { { phazite::BeatPosition::fromBeats (0.0), phazite::decibelsToFaderPosition (0.0f) },
                           { phazite::BeatPosition::fromBeats (4.0), phazite::decibelsToFaderPosition (-12.0f) } });
        phazite::insertSection (*phazite::findSong (s.context), phazite::BeatPosition(),
                                { .name = "Intro", .length = phazite::BeatDuration::fromBeats (12.0) });
    }

    phazite::DawProjectContent describe()
    {
        auto& song = *phazite::findSong (s.context);
        song.storeDeviceStates();
        return phazite::describeDawProject (song, "Test", "A comment");
    }
};
} // namespace

TEST_CASE ("A song as a DAWproject: its tracks, clips and notes, channels and sends, tempo, markers and automation")
{
    Song song;
    auto content = song.describe();
    const auto& project = *content.project;
    CHECK (project.getTagName() == "Project");
    CHECK (project.getStringAttribute ("version") == "1.0");
    CHECK (project.getChildByName ("Application")->getStringAttribute ("name") == "Phazite");

    // The transport.
    const auto* transport = project.getChildByName ("Transport");
    REQUIRE (transport != nullptr);
    CHECK (transport->getChildByName ("Tempo")->getStringAttribute ("value") == "128");
    CHECK (transport->getChildByName ("Tempo")->getStringAttribute ("unit") == "bpm");
    CHECK (transport->getChildByName ("TimeSignature")->getIntAttribute ("numerator") == 3);
    CHECK (transport->getChildByName ("TimeSignature")->getIntAttribute ("denominator") == 4);

    // A track: its channel's volume as a linear gain, its pan from 0 to 1, where its sound goes, and its send.
    const auto* bass = find (project, "Track", "id", id ("t", song.bass));
    REQUIRE (bass != nullptr);
    CHECK (bass->getStringAttribute ("name") == "Bass");
    CHECK (bass->getStringAttribute ("contentType") == "notes");
    const auto* channel = bass->getChildByName ("Channel");
    REQUIRE (channel != nullptr);
    CHECK (channel->getStringAttribute ("role") == "regular");
    CHECK (channel->getStringAttribute ("destination") == "cmaster");
    CHECK (channel->getChildByName ("Volume")->getDoubleAttribute ("value") == doctest::Approx (0.501187).epsilon (0.0001));
    CHECK (channel->getChildByName ("Pan")->getDoubleAttribute ("value") == doctest::Approx (0.75));
    CHECK (channel->getChildByName ("Mute")->getStringAttribute ("value") == "false");
    const auto* send = find (*channel, "Send", "destination", id ("c", song.echo));
    REQUIRE (send != nullptr);
    CHECK (send->getChildByName ("Volume")->getDoubleAttribute ("value") == doctest::Approx (0.251189).epsilon (0.001));

    // Its children in the schema's order.
    juce::StringArray order;

    for (const auto* child : channel->getChildIterator())
        order.add (child->getTagName());

    CHECK (order == juce::StringArray ("Mute", "Pan", "Sends", "Volume"));

    // Its clip and notes, in beats, velocity from 0 to 1.
    const auto* lanes = find (project, "Lanes", "track", id ("t", song.bass));
    REQUIRE (lanes != nullptr);
    const auto* clip = lanes->getChildByName ("Clips")->getChildByName ("Clip");
    REQUIRE (clip != nullptr);
    CHECK (clip->getStringAttribute ("time") == "0");
    CHECK (clip->getStringAttribute ("duration") == "4");
    const auto* notes = clip->getChildByName ("Notes");
    REQUIRE (notes != nullptr);
    REQUIRE (notes->getNumChildElements() == 2);
    const auto* note = find (*notes, "Note", "key", "43");
    REQUIRE (note != nullptr);
    CHECK (note->getStringAttribute ("time") == "2");
    CHECK (note->getStringAttribute ("duration") == "0.5");
    CHECK (note->getDoubleAttribute ("vel") == doctest::Approx (1.0));

    // The fader's automation, as points on its volume.
    const auto* points = find (*lanes, "Points", "id", "q" + id ("c", song.bass) + "v");
    REQUIRE (points != nullptr);
    CHECK (points->getChildByName ("Target")->getStringAttribute ("parameter") == id ("c", song.bass) + "v");
    REQUIRE (points->getNumChildElements() == 3);
    CHECK (points->getChildElement (2)->getDoubleAttribute ("value") == doctest::Approx (0.251189).epsilon (0.001));

    // The section, as a marker; the master; and the metadata.
    const auto* marker = find (project, "Marker", "name", "Intro");
    REQUIRE (marker != nullptr);
    CHECK (marker->getStringAttribute ("time") == "0");
    const auto* master = find (project, "Track", "id", "tmaster");
    REQUIRE (master != nullptr);
    CHECK (master->getChildByName ("Channel")->getStringAttribute ("role") == "master");
    CHECK (content.metadata->getChildByName ("Title")->getAllSubText() == "Test");
}

TEST_CASE ("What another DAW can't play is bounced beside it: the track muted with its notes, its sound on an audio track")
{
    Song song;
    auto content = song.describe();
    const auto& project = *content.project;

    // Phazite's synth on Lead, and on the return: each bounced, the return's sends kept.
    REQUIRE (content.bounces.size() == 2);
    CHECK (content.bounces[0].first == song.lead);
    CHECK (content.bounces[1].first == song.echo);

    const auto* lead = find (project, "Track", "id", id ("t", song.lead));
    REQUIRE (lead != nullptr);
    CHECK (lead->getChildByName ("Channel")->getChildByName ("Mute")->getStringAttribute ("value") == "true");
    CHECK (lead->getChildByName ("Channel")->getChildByName ("Devices") == nullptr);
    const auto* bounce = find (project, "Track", "id", id ("t", song.lead) + "b");
    REQUIRE (bounce != nullptr);
    CHECK (bounce->getStringAttribute ("name") == "Lead (bounce)");
    CHECK (bounce->getStringAttribute ("contentType") == "audio");
    CHECK (find (project, "File", "path", "bounces/" + juce::String ((juce::int64) song.lead.getRaw()) + ".wav") != nullptr);
    CHECK (find (*find (project, "Lanes", "track", id ("t", song.lead)), "Note", "key", "60") != nullptr); // its notes, kept

    // The return's channel stays an effect channel, muted, its sends fed into it; its bounce plays what they sent through it.
    const auto* echo = find (project, "Track", "id", id ("t", song.echo));
    REQUIRE (echo != nullptr);
    CHECK (echo->getChildByName ("Channel")->getStringAttribute ("role") == "effect");
    CHECK (echo->getChildByName ("Channel")->getChildByName ("Mute")->getStringAttribute ("value") == "true");
    const auto* echoBounce = find (project, "Track", "id", id ("t", song.echo) + "b");
    REQUIRE (echoBounce != nullptr);
    CHECK (echoBounce->getChildByName ("Channel")->getStringAttribute ("role") == "regular");
    CHECK (content.metadata->getChildByName ("Comment")->getAllSubText() == "A comment"); // nothing else left out
}

TEST_CASE ("Export as DAWproject writes a zip of project.xml, metadata.xml, the clips' audio and the bounces, of a version too")
{
    Song song;
    TemporaryFolder sounds;
    const auto drums = song.s.addTrack ("Drums");
    song.s.addAudioClip (drums, writeSound (sounds.folder.getChildFile ("Loop.wav"), 1.0), 8.0);
    const auto first = keepVersion (song.s.context, "First");
    song.s.setTempo (90.0);

    phazite::CommandRegistry commands;
    phazite::addVersionCommands (commands, song.s.context, [] (std::function<void (bool)> then) { then (true); }, song.s.renders);
    const auto exportOf = [&song, &commands] (const juce::String& version)
    {
        phazite::selectVersion (song.s.context, version);
        song.s.renders.finished.clear();
        song.s.renders.wrote.clear();
        REQUIRE (commands.invoke ("version.exportDawProject"));
        REQUIRE (dispatchMessagesUntil ([&song] { return ! song.s.renders.finished.isEmpty(); }, 120000));
        INFO (song.s.renders.finished.joinIntoString ("\n").toStdString());
        REQUIRE (song.s.renders.finished[0].endsWith (": ok"));
        REQUIRE (song.s.renders.wrote.size() == 1);
        return song.s.renders.wrote[0];
    };

    // The song as it is.
    CHECK (phazite::getCurrentName (*commands.find ("version.exportDawProject")) == "Export as DAWproject...");
    const auto now = exportOf ({});
    CHECK (now.getFileName() == song.s.getProject().getFile().getFileNameWithoutExtension() + ".dawproject");
    CHECK (now.getParentDirectory().getFileName() == "Renders");

    juce::ZipFile zip (now);
    juce::StringArray entries;

    for (int i = 0; i < zip.getNumEntries(); ++i)
        entries.add (zip.getEntry (i)->filename);

    CHECK (entries.contains ("project.xml"));
    CHECK (entries.contains ("metadata.xml"));
    CHECK (entries.contains ("audio/Loop.wav"));
    CHECK (entries.contains ("bounces/" + juce::String ((juce::int64) song.lead.getRaw()) + ".wav"));

    const std::unique_ptr<juce::InputStream> projectStream (zip.createStreamForEntry (entries.indexOf ("project.xml")));
    REQUIRE (projectStream != nullptr);
    const auto project = juce::parseXML (projectStream->readEntireStreamAsString());
    REQUIRE (project != nullptr);
    CHECK (project->getChildByName ("Transport")->getChildByName ("Tempo")->getStringAttribute ("value") == "90");

    // The bounce's clip, as long as its file.
    const auto* bounceClip = find (*find (*project, "Lanes", "track", id ("t", song.lead) + "b"), "Clip", "time", "0");
    REQUIRE (bounceClip != nullptr);
    CHECK (bounceClip->getDoubleAttribute ("duration") > 4.0);
    CHECK (bounceClip->getChildByName ("Audio")->getDoubleAttribute ("duration") > 1.0);

    // An old version: its own song, named for it, its comment naming it.
    phazite::selectVersion (song.s.context, first);
    CHECK (phazite::getCurrentName (*commands.find ("version.exportDawProject")) == "Export \"First\" as DAWproject...");
    const auto old = exportOf (first);
    CHECK (old.getFileName().endsWith (" - First.dawproject"));
    juce::ZipFile oldZip (old);
    const std::unique_ptr<juce::InputStream> oldProject (oldZip.createStreamForEntry (oldZip.getIndexOfFileName ("project.xml")));
    const std::unique_ptr<juce::InputStream> oldMetadata (oldZip.createStreamForEntry (oldZip.getIndexOfFileName ("metadata.xml")));
    REQUIRE (oldProject != nullptr);
    REQUIRE (oldMetadata != nullptr);
    CHECK (juce::parseXML (oldProject->readEntireStreamAsString())
               ->getChildByName ("Transport")
               ->getChildByName ("Tempo")
               ->getStringAttribute ("value")
           == "128");
    CHECK (oldMetadata->readEntireStreamAsString().contains ("(" + first + ")"));
}

namespace
{
// What another DAW does to a DAWproject as it saves it again: its elements' ids its own ("id0" on), and its numbers to six
// decimals.
void saveAsAnotherDaw (juce::XmlElement& project)
{
    std::map<juce::String, juce::String> ids;
    const std::function<void (juce::XmlElement&)> collect = [&] (juce::XmlElement& element)
    {
        if (element.hasAttribute ("id"))
            ids.emplace (element.getStringAttribute ("id"), "id" + juce::String ((int) ids.size()));

        for (auto* child : element.getChildIterator())
            collect (*child);
    };

    const std::function<void (juce::XmlElement&)> rewrite = [&] (juce::XmlElement& element)
    {
        for (const auto* name : { "id", "destination", "track", "parameter" })
            if (const auto found = ids.find (element.getStringAttribute (name)); element.hasAttribute (name) && found != ids.end())
                element.setAttribute (name, found->second);

        for (const auto* name : { "value", "time", "duration", "vel", "playStart", "playStop" })
            if (element.hasAttribute (name) && element.getStringAttribute (name).containsOnly ("0123456789.-"))
                element.setAttribute (name, juce::String (element.getDoubleAttribute (name), 6));

        for (auto* child : element.getChildIterator())
            rewrite (*child);
    };

    collect (project);
    rewrite (project);
}

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}

std::vector<int> getPitches (const phazite::Song& song, phazite::ClipID clip)
{
    std::vector<int> pitches;

    for (const auto& note : phazite::findClipState (song, clip))
        if (note.hasType (phazite::schema::NOTE))
            pitches.push_back ((int) note[phazite::schema::notePitch]);

    std::sort (pitches.begin(), pitches.end());
    return pitches;
}
} // namespace

TEST_CASE ("A DAWproject opens as a new song: its tracks with their notes, audio and faders, its tempo and its markers")
{
    Song song;
    TemporaryFolder sounds;
    const auto drums = song.s.addTrack ("Drums");
    song.s.addAudioClip (drums, writeSound (sounds.folder.getChildFile ("Loop.wav"), 1.0), 8.0);
    auto content = song.describe();
    content.bounces.clear(); // what another DAW would play of Lead, not rendered here
    const auto file = sounds.folder.getChildFile ("Song.dawproject");
    REQUIRE (phazite::writeDawProject (content, {}, file).wasOk());

    const auto read = phazite::readDawProject (file);
    REQUIRE (read.project != nullptr);
    CHECK (read.fromVersion.isEmpty());
    TestSong into;
    const auto said = phazite::takeDawProject (*phazite::findSong (into.context), read);
    const auto& imported = *phazite::findSong (into.context);

    // Its tracks (the bounces Phazite added left out), in place of the new song's own, in the order they were.
    juce::StringArray names, written;
    std::map<juce::String, phazite::TrackID> tracks;

    for (const auto track : phazite::getTrackIDs (imported))
    {
        names.add (phazite::getTrackName (imported, track));
        tracks[names[names.size() - 1]] = track;
    }

    for (const auto track : phazite::getTrackIDs (*phazite::findSong (song.s.context)))
        written.add (phazite::getTrackName (*phazite::findSong (song.s.context), track));

    CHECK (names == written);
    REQUIRE (tracks.count ("Bass") == 1);
    REQUIRE (tracks.count ("Lead") == 1);
    REQUIRE (tracks.count ("Drums") == 1);
    CHECK (phazite::getChannelVolume (imported, tracks["Bass"]) == doctest::Approx (-6.0f).epsilon (0.001));
    CHECK (phazite::getChannelPan (imported, tracks["Bass"]) == doctest::Approx (0.5f));
    CHECK (phazite::findTrackState (imported, tracks["Lead"])[phazite::schema::trackMute]); // muted, beside its bounce

    // Bass's notes; Drums' audio, copied out into the song's Audio folder.
    const auto riff = phazite::findTrackState (imported, tracks["Bass"]).getChildWithName (phazite::schema::CLIP);
    REQUIRE (riff.isValid());
    CHECK (getPitches (imported, phazite::getClipID (riff)) == std::vector<int> { 36, 43 });
    const auto loop = phazite::findTrackState (imported, tracks["Drums"]).getChildWithName (phazite::schema::CLIP);
    REQUIRE (loop.isValid());
    CHECK ((double) loop[phazite::schema::clipStart] == doctest::Approx (8.0));
    CHECK (imported.getFile().getSiblingFile ("Audio").getChildFile ("Loop.wav").existsAsFile());

    // The tempo, the time signature, the section; and what it couldn't take, said.
    CHECK (phazite::getTempo (imported) == doctest::Approx (128.0));
    CHECK (phazite::getTimeSignatureChanges (imported).front().signature.numerator == 3);
    REQUIRE (phazite::getSections (imported).size() == 1);
    CHECK (phazite::getSections (imported).front().name == "Intro");
    CHECK (said.contains ("Bass: its sends"));
}

TEST_CASE ("A DAWproject past what the zip writer holds, 4 GB or 65,535 files, is refused before it is written, saying what to do")
{
    // Known bug 106: JUCE's zip writer keeps sizes and places in 32 bits and the count of files in 16, so such an archive came
    // out damaged and the export said it worked. The sizes as the check is given them, rather than 4 GB of audio.
    using phazite::ArchiveEntry;
    constexpr auto gigabyte = (juce::int64) 1 << 30;
    const ArchiveEntry xml { "project.xml", 1 << 20, true };

    CHECK (phazite::checkArchiveFits ({ xml, { "audio/Long.wav", 3 * gigabyte, false } }).wasOk());
    const auto tooLarge =
        phazite::checkArchiveFits ({ xml, { "audio/A.wav", 2 * gigabyte, false }, { "audio/B.wav", 2 * gigabyte, false } });
    CHECK (tooLarge.failed());
    CHECK (tooLarge.getErrorMessage().contains ("4 GB"));
    CHECK (tooLarge.getErrorMessage().contains ("Export"));

    // One entry of 4 GB, with its headers; a packed one that deflate could grow past it; and just under.
    CHECK (phazite::checkArchiveFits ({ { "audio/Huge.wav", phazite::maxArchiveBytes, false } }).failed());
    CHECK (phazite::checkArchiveFits ({ { "project.xml", phazite::maxArchiveBytes - 1000, true } }).failed());
    CHECK (phazite::checkArchiveFits ({ { "audio/Huge.wav", phazite::maxArchiveBytes - 1000, false } }).wasOk());

    // Files counted: 65,535 at most.
    std::vector<ArchiveEntry> many ((size_t) phazite::maxArchiveEntries, { "plugins/p.vstpreset", 10, true });
    CHECK (phazite::checkArchiveFits (many).wasOk());
    many.push_back ({ "plugins/q.vstpreset", 10, true });
    CHECK (phazite::checkArchiveFits (many).failed());

    // And the export checks before it writes anything: one with too many files leaves no file.
    Song song;
    auto content = song.describe();
    content.bounces.clear();

    for (int i = 0; i <= phazite::maxArchiveEntries; ++i)
        content.blobs.emplace_back ("plugins/" + juce::String (i) + ".vstpreset", juce::MemoryBlock (4, true));

    TemporaryFolder folder;
    const auto file = folder.folder.getChildFile ("Song.dawproject");
    const auto written = phazite::writeDawProject (content, {}, file);
    CHECK (written.failed());
    CHECK (written.getErrorMessage().contains (juce::String (phazite::maxArchiveEntries)));
    CHECK_FALSE (file.exists());
    CHECK (folder.folder.findChildFiles (juce::File::findFiles, true).isEmpty());
}

TEST_CASE ("A DAWproject's counts that a reader takes for ZIP64's are refused too, and a song too large is refused before it renders")
{
    // A reviewer, of known bug 106's fix: 0xFFFF entries or 0xFFFFFFFF bytes are what readers (Java's, Bitwig Studio's) take
    // for the marker of ZIP64; and the refusal came after minutes of rendering the bounces.
    using phazite::ArchiveEntry;
    std::vector<ArchiveEntry> entries ((size_t) 0xffff, { "plugins/p.vstpreset", 10, true });
    CHECK (phazite::checkArchiveFits (entries).failed());
    entries.pop_back();
    CHECK (phazite::checkArchiveFits (entries).wasOk());
    CHECK (phazite::checkArchiveFits ({ { "a.wav", 0xffffffffLL - 22 - 76 - 10, false } }).failed()); // 0xFFFFFFFF bytes in all

    // The song's bounces, as long as they render: three hours of two channels of 32 bits pass 4 GB; ten seconds don't.
    Song song;
    auto content = song.describe();
    REQUIRE_FALSE (content.bounces.empty()); // the lead's synth, which another DAW can't play
    CHECK (phazite::checkDawProjectFits (content, 10.0).wasOk());
    const auto tooLong = phazite::checkDawProjectFits (content, 3.0 * 3600.0);
    CHECK (tooLong.failed());
    CHECK (tooLong.getErrorMessage().contains ("4 GB"));

    // And the export of such a song says so at once, rendering nothing.
    song.s.addNote (song.s.addMidiClip (song.lead, 30000.0, 4.0), 60, 30000.0, 1.0); // four hours along at 120 BPM
    phazite::CommandRegistry commands;
    phazite::addVersionCommands (commands, song.s.context, [] (std::function<void (bool)> then) { then (true); }, song.s.renders);
    REQUIRE (commands.invoke ("version.exportDawProject"));
    REQUIRE (dispatchMessagesUntil ([&song] { return ! song.s.renders.finished.isEmpty(); }));
    CHECK (song.s.renders.finished[0].contains ("4 GB"));
    CHECK (song.s.renders.wrote.isEmpty());
}

TEST_CASE ("A DAWproject's long exponents are made readable only where a number is read: a device's ID that looks like one stays")
{
    // A reviewer, of known bug 170's fix: every attribute but a name was rewritten, a VST3's device ID of 32 hexadecimal
    // digits starting "5E1234567890" among them.
    Song song;
    auto content = song.describe();
    content.bounces.clear();
    const juce::String deviceID ("5E123456789012345678901234567890");
    auto* plugin = content.project->createNewChildElement ("Vst3Plugin");
    plugin->setAttribute ("deviceID", deviceID);
    plugin->setAttribute ("deviceName", "1e12345678901");
    plugin->setAttribute ("time", "1e12345678901");
    TemporaryFolder folder;
    const auto file = folder.folder.getChildFile ("Song.dawproject");
    REQUIRE (phazite::writeDawProject (content, {}, file).wasOk());

    const auto read = phazite::readDawProject (file);
    REQUIRE (read.project != nullptr);
    const auto* readPlugin = read.project->getChildByName ("Vst3Plugin");
    REQUIRE (readPlugin != nullptr);
    CHECK (readPlugin->getStringAttribute ("deviceID") == deviceID);
    CHECK (readPlugin->getStringAttribute ("deviceName") == "1e12345678901");
    CHECK (readPlugin->getStringAttribute ("time") == "1e999");
}
TEST_CASE ("A DAWproject carries the tempo's changes both ways: steps held, ramps straight, and bent ones in straight pieces")
{
    // From 128 BPM, a ramp down to 96 at beat 8, bent up to 140 at beat 16, held, and a step to 110 at beat 24.
    Song song;
    TemporaryFolder folder;
    auto& written = *phazite::findSong (song.s.context);
    phazite::takeTempoChanges (written,
                               { { phazite::BeatPosition(), 128.0, phazite::tempoRamp },
                                 { phazite::BeatPosition::fromBeats (8.0), 96.0, 0.3f },
                                 { phazite::BeatPosition::fromBeats (16.0), 140.0, phazite::tempoJump },
                                 { phazite::BeatPosition::fromBeats (24.0), 110.0, phazite::tempoJump } },
                               phazite::BeatPosition());
    REQUIRE (phazite::getTempoChanges (written).size() == 4);

    auto content = song.describe();
    const auto* automation = find (*content.project, "TempoAutomation", "id", "tempoAutomation");
    REQUIRE (automation != nullptr);
    CHECK (automation->getChildByName ("Target")->getStringAttribute ("parameter") == "tempo");
    CHECK (automation->getStringAttribute ("timeUnit") == "beats");

    std::map<double, std::pair<double, juce::String>> points; // by time: the tempo and how it goes on

    for (const auto* point : automation->getChildWithTagNameIterator ("RealPoint"))
        points[point->getDoubleAttribute ("time")] = { point->getDoubleAttribute ("value"), point->getStringAttribute ("interpolation") };

    CHECK (points[0.0] == std::pair (128.0, juce::String ("linear")));
    CHECK (points[8.0] == std::pair (96.0, juce::String ("linear")));
    CHECK (points[16.0] == std::pair (140.0, juce::String ("hold")));
    CHECK (points[24.0] == std::pair (110.0, juce::String ("hold")));
    CHECK (points.size() >= 4 + 15); // the bent ramp in at least 16 pieces

    // Read back into a new song: the same tempo all along it, give or take what the straight pieces leave of the bend.
    const auto file = folder.folder.getChildFile ("Tempo.dawproject");
    content.bounces.clear();
    REQUIRE (phazite::writeDawProject (content, {}, file).wasOk());
    TestSong into;
    phazite::takeDawProject (*phazite::findSong (into.context), phazite::readDawProject (file));
    const auto& imported = *phazite::findSong (into.context);

    for (auto beat = 0.0; beat < 30.0; beat += 0.25)
    {
        CAPTURE (beat);
        CHECK (phazite::getTempoAt (imported, phazite::BeatPosition::fromBeats (beat))
               == doctest::Approx (phazite::getTempoAt (written, phazite::BeatPosition::fromBeats (beat))).epsilon (0.01));
    }

    CHECK (phazite::toSeconds (imported, phazite::BeatPosition::fromBeats (30.0))
           == doctest::Approx (phazite::toSeconds (written, phazite::BeatPosition::fromBeats (30.0))).epsilon (0.001));

    // In seconds, as another DAW may write them: each point's beat from the tempos before it.
    auto inSeconds = std::make_unique<juce::XmlElement> (*content.project);
    auto* secondsAutomation = inSeconds->getChildByName ("Arrangement")->getChildByName ("TempoAutomation");
    secondsAutomation->deleteAllChildElementsWithTagName ("RealPoint");
    secondsAutomation->setAttribute ("timeUnit", "seconds");

    for (const auto& [time, bpm, ramps] : { std::tuple (0.0, 120.0, false), std::tuple (2.0, 60.0, true), std::tuple (4.0, 120.0, false) })
    {
        auto* point = secondsAutomation->createNewChildElement ("RealPoint");
        point->setAttribute ("time", time);
        point->setAttribute ("value", bpm);
        point->setAttribute ("interpolation", ramps ? "linear" : "hold");
    }

    phazite::DawProjectFile other;
    other.project = std::move (inSeconds);
    TestSong fromSeconds;
    phazite::takeDawProject (*phazite::findSong (fromSeconds.context), other);
    const auto changes = phazite::getTempoChanges (*phazite::findSong (fromSeconds.context));
    REQUIRE (changes.size() == 3);
    CHECK (changes[1].beat.inBeats() == doctest::Approx (4.0)); // two seconds at 120
    CHECK (changes[2].beat.inBeats() == doctest::Approx (7.0)); // two seconds ramping from 60 to 120, at 90 on average
    CHECK (changes[1].curve == doctest::Approx (phazite::tempoRamp));
}

TEST_CASE ("Import DAWproject opens one from elsewhere as a new untitled project")
{
    TemporaryFolder folder;
    const auto file = folder.folder.getChildFile ("Elsewhere.dawproject");

    {
        Song song;
        auto content = song.describe();
        content.bounces.clear();
        REQUIRE (phazite::writeDawProject (content, {}, file).wasOk());
    }

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (getEngineContext(), recent, folder.folder, folder.folder.getChildFile ("Unsaved"));
    juce::StringArray notices; // what the import left out, which without a listener would be a dialog left open
    session.onNotice = [&notices] (const juce::String& title, const juce::String& message) { notices.add (title + ": " + message); };
    session.importDawProject (file);
    dispatchPendingMessages();

    REQUIRE (session.getProject() != nullptr);
    CHECK (session.isUntitled());
    CHECK (notices.size() <= 1);
    juce::StringArray names;

    for (const auto track : phazite::getTrackIDs (session.getProject()->getSong()))
        names.add (phazite::getTrackName (session.getProject()->getSong(), track));

    CHECK (names.contains ("Bass"));
    CHECK (names.contains ("Lead"));
    CHECK (phazite::getTempo (session.getProject()->getSong()) == doctest::Approx (128.0));
}

TEST_CASE ("Changed in another DAW and imported back: a branch from its version with just those changes, which merges")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass"), drums = s.addTrack ("Drums");
    const auto riff = s.addMidiClip (bass, 0.0, 4.0);
    s.addNote (riff, 36, 0.0, 1.0);
    s.addNote (riff, 40, 1.0 / 3.0, 0.5); // more decimals than another DAW keeps
    s.setVolume (drums, -4.5f);
    const auto first = keepVersion (s.context, "First");
    const auto keptNote = [&s, riff] (int pitch)
    {
        for (const auto& note : phazite::findClipState (*phazite::findSong (s.context), riff))
            if (note.hasType (phazite::schema::NOTE) && (int) note[phazite::schema::notePitch] == pitch)
                return note.createCopy();

        return juce::ValueTree();
    };
    const auto low = keptNote (36);

    // Exported, then saved again by another DAW, which moved a note of Bass up and turned Drums down.
    auto content = phazite::describeDawProject (*phazite::findSong (s.context), "Test",
                                                "Exported by Phazite from the version \"First\" (" + first + ").");
    auto* moved = const_cast<juce::XmlElement*> (find (*find (*content.project, "Lanes", "track", id ("t", bass)), "Note", "key", "40"));
    REQUIRE (moved != nullptr);
    moved->setAttribute ("key", 41);
    auto* fader = const_cast<juce::XmlElement*> (
        find (*find (*content.project, "Track", "id", id ("t", drums)), "Volume", "id", id ("c", drums) + "v"));
    REQUIRE (fader != nullptr);
    fader->setAttribute ("value", 0.316228); // -10 dB
    saveAsAnotherDaw (*content.project);
    TemporaryFolder folder;
    const auto file = folder.folder.getChildFile ("Back.dawproject");
    REQUIRE (phazite::writeDawProject (content, {}, file).wasOk());

    const auto read = phazite::readDawProject (file);
    REQUIRE (read.fromVersion == first);
    juce::StringArray said;
    REQUIRE (phazite::importDawProjectAsBranch (s.context, read, s.save, said));
    CHECK (phazite::getCurrentBranch (s.context)->name == "idea-2");

    // Those two changes, and nothing else: the rest as it was, to the bit.
    const auto changes = phazite::getChangesSinceLastVersion (s.context);
    juce::String listed;

    for (const auto& change : changes)
        listed << change.name << ": " << change.what << "\n";

    INFO (listed.toStdString());
    REQUIRE (changes.size() == 2);
    CHECK (changes[0].name == "Bass");
    CHECK (changes[1].name == "Drums");
    CHECK (changes[1].what == "volume");
    auto& song = *phazite::findSong (s.context);
    CHECK (phazite::getChannelVolume (song, drums) == doctest::Approx (-10.0f).epsilon (0.001));
    CHECK (getPitches (song, riff) == std::vector<int> { 36, 41 });
    CHECK (keptNote (36).isEquivalentTo (low)); // its ID and its values, as they were

    // Kept as a version there, it merges into main with those two lines and no clash.
    const auto back = keepVersion (s.context, "From another DAW");
    REQUIRE (phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save));
    REQUIRE (phazite::startMerge (s.context, back));
    const auto lines = phazite::getMergeLines (s.context);
    REQUIRE (lines.size() == 2);
    CHECK_FALSE (lines[0].clashes);
    CHECK_FALSE (lines[1].clashes);
}

//==============================================================================
// DAWprojects as other DAWs write them. No file that another DAW wrote is in the repository (none may come from a product),
// so these are written by hand, small, from the format's public specification (github.com/bitwig/dawproject: Project.xsd,
// and its README's example, which Bitwig Studio 5.0 wrote) and shaped as the programs that write them do as far as that says:
// their elements in the same order, their units, their ids ("id0" on), six decimals, and what they add that Phazite doesn't
// read. Each audio file in them is a sound of the tests' own. The real thing is a hand check (docs/hand-checks.md).
namespace
{
// A DAWproject's zip: its project.xml, and each audio file under its path there.
juce::File writeArchive (const juce::File& file, const juce::String& projectXml,
                         const std::vector<std::pair<juce::String, juce::File>>& audio)
{
    juce::ZipFile::Builder zip;
    zip.addEntry (std::make_unique<juce::MemoryInputStream> (projectXml.toRawUTF8(), projectXml.getNumBytesAsUTF8(), true), 9,
                  "project.xml", juce::Time::getCurrentTime());
    const juce::String metadata ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<MetaData/>\n");
    zip.addEntry (std::make_unique<juce::MemoryInputStream> (metadata.toRawUTF8(), metadata.getNumBytesAsUTF8(), true), 9, "metadata.xml",
                  juce::Time::getCurrentTime());

    for (const auto& [path, source] : audio)
        zip.addFile (source, 0, path);

    juce::FileOutputStream out (file);
    REQUIRE (out.openedOk());
    REQUIRE (zip.writeToStream (out, nullptr));
    return file;
}

// Shaped as Bitwig Studio 5 writes one, after its README's example: a track on a CLAP instrument with a clip of notes, and a
// group of a drum loop played twice, each audio clip a timeline of its own (Clips in a Clip) whose event stretches the loop,
// recorded at 170 BPM, to the project's 149 by two warps (Warps, in seconds of the file against beats).
juce::String getBitwigProject (double loopSeconds)
{
    const auto seconds = juce::String (loopSeconds, 6);
    juce::String clips;

    for (const auto* time : { "0.0", "8.0" })
        clips << "            <Clip time=\"" << time
              << "\" duration=\"8.0\" playStart=\"0.0\" loopStart=\"0.0\" loopEnd=\"8.0\" "
                 "fadeTimeUnit=\"beats\" fadeInTime=\"0.0\" fadeOutTime=\"0.0\" name=\"Drumfunk3 170bpm\">\n"
                 "              <Clips id=\"id100"
              << time[0]
              << "\">\n"
                 "                <Clip time=\"0.0\" duration=\"8.0\" contentTimeUnit=\"beats\" playStart=\"0.0\" fadeTimeUnit=\"beats\" "
                 "fadeInTime=\"0.0\" fadeOutTime=\"0.0\">\n"
                 "                  <Warps contentTimeUnit=\"seconds\" timeUnit=\"beats\" id=\"id200"
              << time[0] << "\">\n"
              << "                    <Audio algorithm=\"stretch\" channels=\"1\" duration=\"" << seconds
              << "\" sampleRate=\"44100\" id=\"id300" << time[0]
              << "\">\n"
                 "                      <File path=\"audio/Drumfunk3 170bpm.wav\"/>\n"
                 "                    </Audio>\n"
                 "                    <Warp time=\"0.0\" contentTime=\"0.0\"/>\n"
                 "                    <Warp time=\"8.0\" contentTime=\""
              << seconds
              << "\"/>\n"
                 "                  </Warps>\n"
                 "                </Clip>\n"
                 "              </Clips>\n"
                 "            </Clip>\n";

    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<Project version=\"1.0\">\n"
           "  <Application name=\"Bitwig Studio\" version=\"5.0\"/>\n"
           "  <Transport>\n"
           "    <Tempo max=\"666.000000\" min=\"20.000000\" unit=\"bpm\" value=\"149.000000\" id=\"id0\" name=\"Tempo\"/>\n"
           "    <TimeSignature denominator=\"4\" numerator=\"4\" id=\"id1\"/>\n"
           "  </Transport>\n"
           "  <Structure>\n"
           "    <Track contentType=\"notes\" loaded=\"true\" id=\"id2\" name=\"Bass\" color=\"#a2eabf\">\n"
           "      <Channel audioChannels=\"2\" destination=\"id15\" role=\"regular\" solo=\"false\" id=\"id3\">\n"
           "        <Devices>\n"
           "          <ClapPlugin deviceID=\"org.surge-synth-team.surge-xt\" deviceName=\"Surge XT\" deviceRole=\"instrument\" "
           "loaded=\"true\" id=\"id7\" name=\"Surge XT\">\n"
           "            <Parameters/>\n"
           "            <Enabled value=\"true\" id=\"id8\" name=\"On/Off\"/>\n"
           "            <State path=\"plugins/d19b1f6e-bbb6-42fe-a6c9-54b41d97a05d.clap-preset\"/>\n"
           "          </ClapPlugin>\n"
           "        </Devices>\n"
           "        <Mute value=\"false\" id=\"id6\" name=\"Mute\"/>\n"
           "        <Pan max=\"1.000000\" min=\"0.000000\" unit=\"normalized\" value=\"0.500000\" id=\"id5\" name=\"Pan\"/>\n"
           "        <Volume max=\"2.000000\" min=\"0.000000\" unit=\"linear\" value=\"0.659140\" id=\"id4\" name=\"Volume\"/>\n"
           "      </Channel>\n"
           "    </Track>\n"
           "    <Track contentType=\"tracks\" loaded=\"true\" id=\"id40\" name=\"Drums\" color=\"#b53bba\">\n"
           "      <Channel audioChannels=\"2\" destination=\"id15\" role=\"submix\" solo=\"false\" id=\"id41\">\n"
           "        <Mute value=\"false\" id=\"id44\" name=\"Mute\"/>\n"
           "        <Pan max=\"1.000000\" min=\"0.000000\" unit=\"normalized\" value=\"0.500000\" id=\"id43\" name=\"Pan\"/>\n"
           "        <Volume max=\"2.000000\" min=\"0.000000\" unit=\"linear\" value=\"1.000000\" id=\"id42\" name=\"Volume\"/>\n"
           "      </Channel>\n"
           "      <Track contentType=\"audio\" loaded=\"true\" id=\"id9\" name=\"Drumloop\" color=\"#b53bba\">\n"
           "        <Channel audioChannels=\"2\" destination=\"id41\" role=\"regular\" solo=\"false\" id=\"id10\">\n"
           "          <Mute value=\"false\" id=\"id13\" name=\"Mute\"/>\n"
           "          <Pan max=\"1.000000\" min=\"0.000000\" unit=\"normalized\" value=\"0.500000\" id=\"id12\" name=\"Pan\"/>\n"
           "          <Volume max=\"2.000000\" min=\"0.000000\" unit=\"linear\" value=\"0.177125\" id=\"id11\" name=\"Volume\"/>\n"
           "        </Channel>\n"
           "      </Track>\n"
           "    </Track>\n"
           "    <Track contentType=\"audio notes\" loaded=\"true\" id=\"id14\" name=\"Master\">\n"
           "      <Channel audioChannels=\"2\" role=\"master\" solo=\"false\" id=\"id15\">\n"
           "        <Mute value=\"false\" id=\"id18\" name=\"Mute\"/>\n"
           "        <Pan max=\"1.000000\" min=\"0.000000\" unit=\"normalized\" value=\"0.500000\" id=\"id17\" name=\"Pan\"/>\n"
           "        <Volume max=\"2.000000\" min=\"0.000000\" unit=\"linear\" value=\"1.000000\" id=\"id16\" name=\"Volume\"/>\n"
           "      </Channel>\n"
           "    </Track>\n"
           "  </Structure>\n"
           "  <Arrangement id=\"id19\">\n"
           "    <Lanes timeUnit=\"beats\" id=\"id20\">\n"
           "      <Lanes track=\"id2\" id=\"id21\">\n"
           "        <Clips id=\"id22\">\n"
           "          <Clip time=\"0.0\" duration=\"8.0\" playStart=\"0.0\">\n"
           "            <Notes id=\"id23\">\n"
           "              <Note time=\"0.000000\" duration=\"0.250000\" channel=\"0\" key=\"65\" vel=\"0.787402\" rel=\"0.787402\"/>\n"
           "              <Note time=\"1.500000\" duration=\"2.500000\" channel=\"0\" key=\"53\" vel=\"0.787402\" rel=\"0.787402\"/>\n"
           "            </Notes>\n"
           "          </Clip>\n"
           "        </Clips>\n"
           "      </Lanes>\n"
           "      <Lanes track=\"id9\" id=\"id24\">\n"
           "        <Clips id=\"id25\">\n"
           + clips
           + "        </Clips>\n"
             "      </Lanes>\n"
             "      <Lanes track=\"id14\" id=\"id29\">\n"
             "        <Clips id=\"id30\"/>\n"
             "      </Lanes>\n"
             "    </Lanes>\n"
             "  </Arrangement>\n"
             "  <Scenes/>\n"
             "</Project>\n";
}

// Shaped as Studio One 6.5 writes one, as far as the specification says: an audio track whose clips hold their audio
// directly, in seconds, the same recording played by three of them; a VST3 effect on it, a send to an effect channel, and
// its volume's automation in its lane.
juce::String getStudioOneProject()
{
    juce::String clips;

    for (const auto* time : { "0.000000", "4.000000", "8.000000" })
        clips << "          <Clip time=\"" << time << "\" duration=\"4.000000\" playStart=\"0.000000\" contentTimeUnit=\"seconds\" "
              << "fadeTimeUnit=\"seconds\" fadeInTime=\"0.010000\" fadeOutTime=\"0.010000\" name=\"Vox\">\n"
              << "            <Audio algorithm=\"\" channels=\"1\" duration=\"2.000000\" sampleRate=\"44100\" id=\"id50" << time[0]
              << "\">\n"
                 "              <File path=\"Audio/Vox.wav\"/>\n"
                 "            </Audio>\n"
                 "          </Clip>\n";

    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<Project version=\"1.0\">\n"
           "  <Application name=\"Studio One\" version=\"6.5.2\"/>\n"
           "  <Transport>\n"
           "    <Tempo unit=\"bpm\" value=\"120.000000\" id=\"id0\" name=\"Tempo\"/>\n"
           "    <TimeSignature numerator=\"4\" denominator=\"4\" id=\"id1\"/>\n"
           "  </Transport>\n"
           "  <Structure>\n"
           "    <Track contentType=\"audio\" loaded=\"true\" id=\"id2\" name=\"Vocals\" color=\"#e0533f\">\n"
           "      <Channel audioChannels=\"1\" destination=\"id30\" role=\"regular\" solo=\"false\" id=\"id3\">\n"
           "        <Devices>\n"
           "          <Vst3Plugin deviceID=\"72C4DB717A4D459AB97E51745D84B39D\" deviceName=\"Pro-Q 3\" deviceRole=\"audioFX\" "
           "loaded=\"true\" id=\"id8\" name=\"Pro-Q 3\">\n"
           "            <Enabled value=\"true\" id=\"id9\" name=\"On/Off\"/>\n"
           "            <State path=\"plugins/Pro-Q 3.vstpreset\"/>\n"
           "          </Vst3Plugin>\n"
           "        </Devices>\n"
           "        <Mute value=\"false\" id=\"id4\" name=\"Mute\"/>\n"
           "        <Pan unit=\"normalized\" value=\"0.500000\" min=\"0.000000\" max=\"1.000000\" id=\"id5\" name=\"Pan\"/>\n"
           "        <Sends>\n"
           "          <Send destination=\"id20\" type=\"post\" id=\"id10\">\n"
           "            <Volume unit=\"linear\" value=\"0.500000\" min=\"0.000000\" max=\"2.000000\" id=\"id11\" name=\"Send\"/>\n"
           "          </Send>\n"
           "        </Sends>\n"
           "        <Volume unit=\"linear\" value=\"0.800000\" min=\"0.000000\" max=\"2.000000\" id=\"id6\" name=\"Volume\"/>\n"
           "      </Channel>\n"
           "    </Track>\n"
           "    <Track contentType=\"audio\" loaded=\"true\" id=\"id19\" name=\"Reverb\" color=\"#3f8fe0\">\n"
           "      <Channel audioChannels=\"2\" destination=\"id30\" role=\"effect\" solo=\"false\" id=\"id20\">\n"
           "        <Devices>\n"
           "          <BuiltinDevice deviceName=\"Room Reverb\" deviceRole=\"audioFX\" loaded=\"true\" id=\"id21\" name=\"Room Reverb\">\n"
           "            <Enabled value=\"true\" id=\"id22\" name=\"On/Off\"/>\n"
           "          </BuiltinDevice>\n"
           "        </Devices>\n"
           "        <Mute value=\"false\" id=\"id23\" name=\"Mute\"/>\n"
           "        <Pan unit=\"normalized\" value=\"0.500000\" min=\"0.000000\" max=\"1.000000\" id=\"id24\" name=\"Pan\"/>\n"
           "        <Volume unit=\"linear\" value=\"1.000000\" min=\"0.000000\" max=\"2.000000\" id=\"id25\" name=\"Volume\"/>\n"
           "      </Channel>\n"
           "    </Track>\n"
           "    <Track contentType=\"audio notes\" loaded=\"true\" id=\"id29\" name=\"Main\">\n"
           "      <Channel audioChannels=\"2\" role=\"master\" solo=\"false\" id=\"id30\">\n"
           "        <Mute value=\"false\" id=\"id31\" name=\"Mute\"/>\n"
           "        <Pan unit=\"normalized\" value=\"0.500000\" min=\"0.000000\" max=\"1.000000\" id=\"id32\" name=\"Pan\"/>\n"
           "        <Volume unit=\"linear\" value=\"1.000000\" min=\"0.000000\" max=\"2.000000\" id=\"id33\" name=\"Volume\"/>\n"
           "      </Channel>\n"
           "    </Track>\n"
           "  </Structure>\n"
           "  <Arrangement id=\"id40\">\n"
           "    <Lanes timeUnit=\"beats\" id=\"id41\">\n"
           "      <Lanes track=\"id2\" id=\"id42\">\n"
           "        <Clips id=\"id43\">\n"
           + clips
           + "        </Clips>\n"
             "        <Points unit=\"linear\" id=\"id44\">\n"
             "          <Target parameter=\"id6\"/>\n"
             "          <RealPoint time=\"0.000000\" value=\"0.800000\" interpolation=\"linear\"/>\n"
             "          <RealPoint time=\"8.000000\" value=\"0.400000\" interpolation=\"linear\"/>\n"
             "        </Points>\n"
             "      </Lanes>\n"
             "    </Lanes>\n"
             "  </Arrangement>\n"
             "</Project>\n";
}

juce::StringArray getAudioFiles (const phazite::Song& song)
{
    juce::StringArray names;

    for (const auto& file : song.getFile().getSiblingFile ("Audio").findChildFiles (juce::File::findFiles, true))
        names.add (file.getFileName());

    names.sort (true);
    return names;
}

std::vector<juce::ValueTree> getClipStates (const phazite::Song& song, phazite::TrackID track)
{
    std::vector<juce::ValueTree> clips;

    for (const auto& child : phazite::findTrackState (song, track))
        if (child.hasType (phazite::schema::CLIP))
            clips.push_back (child);

    return clips;
}

phazite::TrackID findTrack (const phazite::Song& song, const juce::String& name)
{
    for (const auto track : phazite::getTrackIDs (song))
        if (phazite::getTrackName (song, track) == name)
            return track;

    return {};
}
} // namespace

TEST_CASE ("A DAWproject shaped as Bitwig Studio writes one: notes, an audio clip in a clip stretched by its warps, a group")
{
    TemporaryFolder folder;
    const auto loopSeconds = 8.0 * 60.0 / 170.0; // two bars at 170 BPM
    const auto loop = writeSound (folder.folder.getChildFile ("Loop.wav"), loopSeconds);
    const auto file = writeArchive (folder.folder.getChildFile ("Bitwig.dawproject"), getBitwigProject (loopSeconds),
                                    { { "audio/Drumfunk3 170bpm.wav", loop } });

    const auto read = phazite::readDawProject (file);
    REQUIRE (read.project != nullptr);
    TestSong into;
    auto& song = *phazite::findSong (into.context);
    const auto said = phazite::takeDawProject (song, read);
    INFO (said.joinIntoString ("\n").toStdString());
    phazite::test::checkSong (song);

    // Its tracks, the group's in it on their own; the tempo; Bass's notes.
    const auto bass = findTrack (song, "Bass"), drumloop = findTrack (song, "Drumloop");
    REQUIRE (bass.isValid());
    REQUIRE (drumloop.isValid());
    CHECK (phazite::getTempo (song) == doctest::Approx (149.0));
    const auto notes = getClipStates (song, bass);
    REQUIRE (notes.size() == 1);
    CHECK (getPitches (song, phazite::getClipID (notes[0])) == std::vector<int> { 53, 65 });

    // The loop twice, from one copy of its file, each clip at its own place and following the tempo at the loop's own, 170.
    const auto clips = getClipStates (song, drumloop);
    REQUIRE (clips.size() == 2);
    CHECK (getAudioFiles (song) == juce::StringArray ("Drumfunk3 170bpm.wav"));

    for (size_t i = 0; i < clips.size(); ++i)
    {
        CAPTURE (i);
        CHECK ((double) clips[i][phazite::schema::clipStart] == doctest::Approx (8.0 * (double) i));
        CHECK (phazite::getClipTempo (song, phazite::getClipID (clips[i])) == doctest::Approx (170.0).epsilon (0.001));
        CHECK (phazite::getClipFile (song, clips[i]).getFileName() == "Drumfunk3 170bpm.wav");
    }

    // What it left out, by track: the instrument, and the group's own channel.
    CHECK (said.joinIntoString ("\n").contains ("Bass: its devices (Surge XT)"));
    CHECK (said.joinIntoString ("\n").contains ("Drums"));
}

TEST_CASE ("A DAWproject shaped as Studio One writes one: one copy of a recording three clips play, and what it left out")
{
    TemporaryFolder folder;
    const auto vox = writeSound (folder.folder.getChildFile ("Vox.wav"), 2.0);
    const auto file =
        writeArchive (folder.folder.getChildFile ("Studio One.dawproject"), getStudioOneProject(), { { "Audio/Vox.wav", vox } });
    const auto read = phazite::readDawProject (file);
    REQUIRE (read.project != nullptr);

    // Imported as the app imports one: a new untitled project, with what it left out told.
    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (getEngineContext(), recent, folder.folder, folder.folder.getChildFile ("Unsaved"));
    juce::StringArray notices;
    session.onNotice = [&notices] (const juce::String& title, const juce::String& message) { notices.add (title + ": " + message); };
    session.importDawProject (file);
    dispatchPendingMessages();
    REQUIRE (session.getProject() != nullptr);
    auto& song = session.getProject()->getSong();
    phazite::test::checkSong (song);

    const auto vocals = findTrack (song, "Vocals");
    REQUIRE (vocals.isValid());
    const auto clips = getClipStates (song, vocals);
    REQUIRE (clips.size() == 3);
    CHECK (getAudioFiles (song) == juce::StringArray ("Vox.wav"));

    for (const auto& clip : clips)
        CHECK (phazite::getClipFile (song, clip).getFileName() == "Vox.wav");

    REQUIRE (notices.size() == 1);
    const auto& told = notices[0];
    INFO (told.toStdString());
    CHECK (told.contains ("Vocals: its devices (Pro-Q 3)"));
    CHECK (told.contains ("Vocals: its sends"));
    CHECK (told.contains ("Vocals: its automation"));
    CHECK (told.contains ("Reverb: its devices (Room Reverb)"));
}

TEST_CASE ("A DAWproject with a note a million beats along, or audio from past where a file can end, opens within a song's ranges")
{
    TemporaryFolder folder;
    const auto loopSeconds = 8.0 * 60.0 / 170.0;
    const auto sound = writeSound (folder.folder.getChildFile ("Sound.wav"), loopSeconds);
    juce::File file;

    SUBCASE ("A note far past its clip's end: the clip that grows to hold it is no longer than a clip can be")
    {
        const auto text = getBitwigProject (loopSeconds).replace ("<Note time=\"1.500000\"", "<Note time=\"1e15\"");
        REQUIRE (text.contains ("1e15"));
        file = writeArchive (folder.folder.getChildFile ("Far note.dawproject"), text, { { "audio/Drumfunk3 170bpm.wav", sound } });
    }

    SUBCASE ("Audio played from past the last second a file can have: its end is that second")
    {
        const auto text = getStudioOneProject().replace ("playStart=\"0.000000\"", "playStart=\"1e15\"");
        REQUIRE (text.contains ("1e15"));
        file = writeArchive (folder.folder.getChildFile ("Far audio.dawproject"), text, { { "Audio/Vox.wav", sound } });
    }

    const auto read = phazite::readDawProject (file);
    REQUIRE (read.project != nullptr);
    TestSong into;
    auto& song = *phazite::findSong (into.context);
    phazite::takeDawProject (song, read);
    phazite::test::checkSong (song);
}
