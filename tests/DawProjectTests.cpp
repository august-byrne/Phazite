// SPDX-License-Identifier: AGPL-3.0-or-later
// DAWproject (model/project/internal/DawProject.h; docs/versions.md: DAWproject with versions, steps D1 and D2): a song
// written as one, in DAWproject's names (Project.xsd, version 1.0), what another DAW can't play bounced beside it, and the
// zip, from the song as it is and from an old version.
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "app/ProjectSession.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
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

namespace
{
using namespace phazite::test;

void waitForVersions (phazite::ProjectContext& context)
{
    REQUIRE (dispatchMessagesUntil (
        [&context]
        {
            const auto* history = phazite::getVersionHistory (context);
            return history == nullptr || ! history->isBusy();
        }));
}

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name)
{
    waitForVersions (context);
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }));
    REQUIRE (result->wasOk());
    waitForVersions (context);
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
        return phazite::describeDawProject (*phazite::findSong (s.context), &phazite::getSession (s.context)->getPlugins(), "Test",
                                            "A comment");
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
    CHECK (said.contains ("sends"));
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
    session.importDawProject (file);
    dispatchPendingMessages();

    REQUIRE (session.getProject() != nullptr);
    CHECK (session.isUntitled());
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
    auto content = phazite::describeDawProject (*phazite::findSong (s.context), nullptr, "Test",
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
    CHECK (phazite::getCurrentBranch (s.context)->name == "Idea 2");

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

    // Kept as a version there, it merges into Main with those two lines and no clash.
    const auto back = keepVersion (s.context, "From another DAW");
    REQUIRE (phazite::switchBranch (s.context, findBranch (s.context, "Main"), s.save));
    REQUIRE (phazite::startMerge (s.context, back));
    const auto lines = phazite::getMergeLines (s.context);
    REQUIRE (lines.size() == 2);
    CHECK_FALSE (lines[0].clashes);
    CHECK_FALSE (lines[1].clashes);
}
