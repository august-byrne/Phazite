// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DawProject.h"
#include "Export.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/engine/AppInfo.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/OfflineRender.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <map>
#include <set>

namespace phazite
{
namespace
{
    constexpr double bounceRate = 48000.0;
    constexpr int bounceBlockSize = 512;

    // A number as DAWproject's files write them: as many decimals as it needs, up to nine.
    juce::String number (double value)
    {
        auto text = juce::String (value, 9);

        if (text.containsChar ('.'))
            text = text.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

        return text == "-0" ? juce::String ("0") : text;
    }

    juce::String toColourText (juce::Colour colour)
    {
        return "#" + colour.toDisplayString (false).toLowerCase();
    }

    // A fader's volume (in its positions) as DAWproject's linear gain, from 0 to 2.
    double toLinear (float decibels)
    {
        return decibels <= -100.0f ? 0.0 : juce::jlimit (0.0, 2.0, std::pow (10.0, decibels / 20.0));
    }

    juce::XmlElement* addParameter (juce::XmlElement& parent, const char* type, const juce::String& id, const juce::String& name)
    {
        auto* parameter = parent.createNewChildElement (type);
        parameter->setAttribute ("id", id);
        parameter->setAttribute ("name", name);
        return parameter;
    }

    void addVolume (juce::XmlElement& channel, const juce::String& id, double linear)
    {
        auto* volume = addParameter (channel, "Volume", id, "Volume");
        volume->setAttribute ("unit", "linear");
        volume->setAttribute ("min", "0");
        volume->setAttribute ("max", "2");
        volume->setAttribute ("value", number (linear));
    }

    void addPan (juce::XmlElement& channel, const juce::String& id, double pan)
    {
        auto* element = addParameter (channel, "Pan", id, "Pan");
        element->setAttribute ("unit", "normalized");
        element->setAttribute ("min", "0");
        element->setAttribute ("max", "1");
        element->setAttribute ("value", number ((pan + 1.0) / 2.0));
    }

    // Whether another DAW plays a device: a VST3 plug-in.
    bool isVst3 (const juce::ValueTree& device)
    {
        return device.hasType (schema::DEVICE) && device[schema::deviceType].toString() == schema::deviceIsPlugin
               && device[schema::pluginFormat].toString() == "VST3";
    }

    // What of a track another DAW can't play, which its bounce plays: why, or nothing.
    juce::String findWhyBounced (const juce::ValueTree& track)
    {
        if ((bool) track[schema::trackFrozen])
            return "frozen";

        for (const auto& child : track)
        {
            if (child.hasType (schema::CONTAINER) || (child.hasType (schema::DEVICE) && ! isVst3 (child)))
                return "devices";

            if ((child.hasType (schema::MODULATOR) || child.hasType (schema::MACRO))
                && child.getChildWithName (schema::MODULATES).isValid())
                return "modulation";

            if (child.hasType (schema::CLIP) && child[schema::clipKind].toString() == schema::clipIsAudio
                && (! juce::approximatelyEqual ((float) child[schema::clipGain], 0.0f) || child.hasProperty (schema::clipTempo)
                    || ! juce::approximatelyEqual ((float) child[schema::clipPitch], 0.0f) || (bool) child[schema::clipReversed]
                    || child.hasProperty (schema::clipTakes)))
                return "audio clips";
        }

        return {};
    }

    //==============================================================================
    struct Writer
    {
        const Song& song;
        PluginInstances* plugins;
        DawProjectContent& content;
        engine::TempoMap tempo { getTempoMap (song) };   // in seconds
        juce::StringArray leftOut;                       // what couldn't go in
        std::map<juce::String, juce::String> audioFiles; // each file's full path: its path in the zip
        juce::XmlElement* lanes = nullptr;               // the arrangement's

        juce::String channelID (std::uint64_t track) const
        {
            return track == 0 ? juce::String ("cmaster") : "c" + juce::String ((juce::int64) track);
        }

        void write (const juce::String& title, const juce::String& comment)
        {
            auto& project = *(content.project = std::make_unique<juce::XmlElement> ("Project"));
            project.setAttribute ("version", "1.0");
            auto* application = project.createNewChildElement ("Application");
            application->setAttribute ("name", appName);
            application->setAttribute ("version", getVersionString());

            writeTransport (*project.createNewChildElement ("Transport"));
            auto& structure = *project.createNewChildElement ("Structure");
            auto& arrangement = *project.createNewChildElement ("Arrangement");
            arrangement.setAttribute ("id", "arrangement");
            lanes = arrangement.createNewChildElement ("Lanes");
            lanes->setAttribute ("timeUnit", "beats");
            lanes->setAttribute ("id", "lanes");

            writeTracks (structure, song.getState().getChildWithName (schema::TRACKS));
            writeMaster (structure);
            writeMarkers (arrangement);
            writeTempoChanges (arrangement);

            auto& metadata = *(content.metadata = std::make_unique<juce::XmlElement> ("MetaData"));
            metadata.createNewChildElement ("Title")->addTextElement (title);
            auto note = comment;

            if (! leftOut.isEmpty())
                note << (note.isNotEmpty() ? "\n" : "") << "Left out: " << leftOut.joinIntoString ("; ") << ".";

            if (note.isNotEmpty())
                metadata.createNewChildElement ("Comment")->addTextElement (note);

            content.tempo = tempo;
        }

        //==============================================================================
        void writeTransport (juce::XmlElement& transport)
        {
            const auto tempos = getTempoChanges (song);
            const auto signatures = getTimeSignatureChanges (song);
            auto* bpm = addParameter (transport, "Tempo", "tempo", "Tempo");
            bpm->setAttribute ("unit", "bpm");
            bpm->setAttribute ("min", "20");
            bpm->setAttribute ("max", "999");
            bpm->setAttribute ("value", number (tempos.front().bpm));
            auto* signature = transport.createNewChildElement ("TimeSignature");
            signature->setAttribute ("numerator", signatures.front().signature.numerator);
            signature->setAttribute ("denominator", signatures.front().signature.denominator);
            signature->setAttribute ("id", "timeSignature");
        }

        void writeTempoChanges (juce::XmlElement& arrangement)
        {
            const auto tempos = getTempoChanges (song);
            const auto signatures = getTimeSignatureChanges (song);

            if (tempos.size() > 1)
            {
                auto* points = arrangement.createNewChildElement ("TempoAutomation");
                points->setAttribute ("unit", "bpm");
                points->setAttribute ("id", "tempoAutomation");
                points->createNewChildElement ("Target")->setAttribute ("parameter", "tempo");

                for (const auto& change : tempos)
                {
                    auto* point = points->createNewChildElement ("RealPoint");
                    point->setAttribute ("time", number (change.beat.inBeats()));
                    point->setAttribute ("value", number (change.bpm));
                    point->setAttribute ("interpolation", "hold");
                }
            }

            if (signatures.size() > 1)
            {
                auto* points = arrangement.createNewChildElement ("TimeSignatureAutomation");
                points->setAttribute ("id", "timeSignatureAutomation");
                points->createNewChildElement ("Target")->setAttribute ("parameter", "timeSignature");

                for (const auto& change : signatures)
                {
                    auto* point = points->createNewChildElement ("TimeSignaturePoint");
                    point->setAttribute ("time", number (change.beat.inBeats()));
                    point->setAttribute ("numerator", change.signature.numerator);
                    point->setAttribute ("denominator", change.signature.denominator);
                }
            }
        }

        void writeMarkers (juce::XmlElement& arrangement)
        {
            std::vector<juce::ValueTree> sections;

            for (const auto& section : song.getState().getChildWithName (schema::SECTIONS))
                if (section.hasType (schema::SECTION))
                    sections.push_back (section);

            if (sections.empty())
                return;

            std::sort (sections.begin(), sections.end(),
                       [] (const auto& a, const auto& b) { return (double) a[schema::sectionStart] < (double) b[schema::sectionStart]; });
            auto* markers = arrangement.createNewChildElement ("Markers");
            markers->setAttribute ("id", "markers");

            for (const auto& section : sections)
            {
                auto* marker = markers->createNewChildElement ("Marker");
                marker->setAttribute ("time", number ((double) section[schema::sectionStart]));
                marker->setAttribute ("name", section[schema::sectionName].toString());

                if (const auto colour = juce::Colour::fromString (section[schema::sectionColour].toString()); ! colour.isTransparent())
                    marker->setAttribute ("color", toColourText (colour));
            }
        }

        //==============================================================================
        void writeTracks (juce::XmlElement& parent, const juce::ValueTree& tracks)
        {
            for (const auto& track : tracks)
                if (track.hasType (schema::TRACK))
                    writeTrack (parent, track);
        }

        void writeTrack (juce::XmlElement& parent, const juce::ValueTree& track)
        {
            const auto id = readID (track[schema::trackID]);
            const auto trackID = TrackID::fromRaw (id);
            const auto name = getTrackName (song, trackID);
            const auto colour = getTrackColour (song, trackID, (int) getTrackRow (trackID));
            const auto isFolder = isFolderState (track), isReturn = isReturnState (track);
            const auto bus = isFolder && (bool) track[schema::folderBus];
            const auto why = isFolder ? juce::String() : findWhyBounced (track);

            auto* element = parent.createNewChildElement ("Track");
            element->setAttribute ("contentType", isFolder ? "tracks" : hasAudioClips (track) ? "audio" : "notes");
            element->setAttribute ("loaded", "true");
            element->setAttribute ("id", "t" + juce::String ((juce::int64) id));
            element->setAttribute ("name", name);
            element->setAttribute ("color", toColourText (colour));

            // A plain folder only gathers its tracks; a bus, a track and a return have a channel.
            if (! isFolder || bus)
                writeChannel (*element, track, id, isReturn ? "effect" : bus ? "submix" : "regular", why.isNotEmpty());

            if (isFolder)
                writeTracks (*element, track);
            else
                writeLanes (track, id, "t" + juce::String ((juce::int64) id));

            // What another DAW can't play, bounced: beside it on an audio track of its own, which plays as its fader did.
            if (why.isNotEmpty())
            {
                const auto bounceID = juce::String ((juce::int64) id) + "b";
                auto* bounced = parent.createNewChildElement ("Track");
                bounced->setAttribute ("contentType", "audio");
                bounced->setAttribute ("loaded", "true");
                bounced->setAttribute ("id", "t" + bounceID);
                bounced->setAttribute ("name", name + " (bounce)");
                bounced->setAttribute ("color", toColourText (colour));
                writeChannelOf (*bounced, track, "c" + bounceID, "regular", false, false); // an effect channel holds no clips

                auto* lane = lanes->createNewChildElement ("Lanes");
                lane->setAttribute ("track", "t" + bounceID);
                lane->setAttribute ("id", "l" + bounceID);
                auto* clips = lane->createNewChildElement ("Clips");
                clips->setAttribute ("id", "k" + bounceID);
                auto* clip = clips->createNewChildElement ("Clip");
                clip->setAttribute ("time", "0");
                clip->setAttribute ("contentTimeUnit", "seconds");
                clip->setAttribute ("playStart", "0");
                clip->setAttribute ("name", name);
                auto* audio = clip->createNewChildElement ("Audio");
                audio->setAttribute ("channels", 2);
                audio->setAttribute ("sampleRate", (int) bounceRate);
                audio->setAttribute ("id", "a" + bounceID);
                audio->createNewChildElement ("File")->setAttribute ("path", "bounces/" + juce::String ((juce::int64) id) + ".wav");
                writeAutomation (*lane, track, "c" + bounceID);
                content.bounces.emplace_back (trackID, clip);
            }
        }

        size_t getTrackRow (TrackID track) const
        {
            const auto tracks = getTrackIDs (song);
            return (size_t) std::distance (tracks.begin(), std::find (tracks.begin(), tracks.end(), track));
        }

        static bool hasAudioClips (const juce::ValueTree& track)
        {
            for (const auto& child : track)
                if (child.hasType (schema::CLIP) && child[schema::clipKind].toString() == schema::clipIsAudio)
                    return true;

            return false;
        }

        // Where a channel's sound goes: its bus's channel, or the master's.
        juce::String findDestination (const juce::ValueTree& track) const
        {
            const auto bus = getOutputBus (song, TrackID::fromRaw (readID (track[schema::trackID])));
            return channelID (bus.isValid() ? bus.getRaw() : 0);
        }

        void writeChannel (juce::XmlElement& element, const juce::ValueTree& track, std::uint64_t id, const char* role, bool bounced)
        {
            // A bounced track keeps its notes, muted: its bounce plays its sound.
            writeChannelOf (element, track, channelID (id), role, ! bounced, bounced);
        }

        void writeChannelOf (juce::XmlElement& element, const juce::ValueTree& track, const juce::String& id, const char* role,
                             bool withDevices, bool muted)
        {
            auto* channel = element.createNewChildElement ("Channel");
            channel->setAttribute ("audioChannels", 2);
            channel->setAttribute ("destination", findDestination (track));
            channel->setAttribute ("role", role);
            channel->setAttribute ("solo", (bool) track[schema::trackSolo] ? "true" : "false");
            channel->setAttribute ("id", id);

            if (withDevices)
                writeDevices (*channel, track, getTrackName (song, TrackID::fromRaw (readID (track[schema::trackID]))));

            auto* mute = addParameter (*channel, "Mute", id + "m", "Mute");
            mute->setAttribute ("value", muted || (bool) track[schema::trackMute] ? "true" : "false");
            addPan (*channel, id + "p", (double) track.getProperty (schema::channelPan, 0.0));
            writeSends (*channel, track, id);
            addVolume (*channel, id + "v", toLinear ((float) track.getProperty (schema::channelVolume, 0.0)));
        }

        void writeSends (juce::XmlElement& channel, const juce::ValueTree& track, const juce::String& id)
        {
            juce::XmlElement* sends = nullptr;

            for (const auto& send : track)
            {
                if (! send.hasType (schema::SEND))
                    continue;

                if (sends == nullptr)
                    sends = channel.createNewChildElement ("Sends");

                const auto sendID = id + "s" + send[schema::sendID].toString();
                auto* element = sends->createNewChildElement ("Send");
                element->setAttribute ("destination", channelID (readID (send[schema::sendTo])));
                element->setAttribute ("type", (bool) send[schema::sendPreFader] ? "pre" : "post");
                element->setAttribute ("id", sendID);
                addVolume (*element, sendID + "v", toLinear ((float) send.getProperty (schema::sendLevel, 0.0)));
            }
        }

        void writeDevices (juce::XmlElement& channel, const juce::ValueTree& track, const juce::String& where)
        {
            juce::XmlElement* devices = nullptr;

            for (const auto& device : track)
            {
                if (! device.hasType (schema::DEVICE) && ! device.hasType (schema::CONTAINER))
                    continue;

                if (! isVst3 (device))
                {
                    leftOut.addIfNotAlreadyThere (where + "'s " + getDeviceName (device));
                    continue;
                }

                if (devices == nullptr)
                    devices = channel.createNewChildElement ("Devices");

                writePlugin (*devices, device);
            }
        }

        void writePlugin (juce::XmlElement& devices, const juce::ValueTree& device)
        {
            const auto id = readID (device[schema::deviceID]);
            const auto elementID = "d" + juce::String ((juce::int64) id);
            auto* plugin = devices.createNewChildElement ("Vst3Plugin");
            const auto preset = takePreset (DeviceID::fromRaw (id));

            // The plug-in's class ID, as its preset holds it (after "VST3" and the preset's version).
            if (preset.getSize() >= 40)
                plugin->setAttribute ("deviceID", juce::String::fromUTF8 (static_cast<const char*> (preset.getData()) + 8, 32));

            plugin->setAttribute ("deviceName", device[schema::pluginName].toString());
            plugin->setAttribute ("deviceRole", (bool) device[schema::pluginInstrument] ? "instrument" : "audioFX");
            plugin->setAttribute ("deviceVendor", device[schema::pluginMaker].toString());
            plugin->setAttribute ("loaded", preset.isEmpty() ? "false" : "true");
            plugin->setAttribute ("id", elementID);
            plugin->setAttribute ("name", device[schema::pluginName].toString());

            // Its automated parameters, which the automation's points name.
            juce::XmlElement* parameters = nullptr;

            for (const auto& curve : song.getState().getChildWithName (schema::AUTOMATION))
            {
                if (! curve.hasType (schema::CURVE) || readID (curve[schema::curveDevice]) != id)
                    continue;

                if (parameters == nullptr)
                    parameters = plugin->createNewChildElement ("Parameters");

                auto* parameter = addParameter (*parameters, "RealParameter", elementID + "p" + curve[schema::curveParameter].toString(),
                                                curve[schema::curveParameter].toString());
                parameter->setAttribute ("parameterID", curve[schema::curveParameter].toString().getIntValue());
                parameter->setAttribute ("unit", "normalized");
                parameter->setAttribute ("min", "0");
                parameter->setAttribute ("max", "1");
            }

            auto* enabled = addParameter (*plugin, "Enabled", elementID + "e", "On/Off");
            enabled->setAttribute ("value", (bool) device[schema::deviceOff] ? "false" : "true");

            if (preset.isEmpty())
            {
                leftOut.addIfNotAlreadyThere (device[schema::pluginName].toString() + "'s settings (it didn't load)");
                return;
            }

            const auto path = "plugins/" + elementID + ".vstpreset";
            plugin->createNewChildElement ("State")->setAttribute ("path", path);
            content.blobs.emplace_back (path, preset);
        }

        juce::MemoryBlock takePreset (DeviceID device) const
        {
            if (plugins == nullptr)
                return {};

            const auto instance = plugins->find (device);
            return instance != nullptr ? instance->getPreset() : juce::MemoryBlock();
        }

        void writeMaster (juce::XmlElement& structure)
        {
            const auto master = song.getState().getChildWithName (schema::MASTER);
            auto* element = structure.createNewChildElement ("Track");
            element->setAttribute ("contentType", "audio notes");
            element->setAttribute ("loaded", "true");
            element->setAttribute ("id", "tmaster");
            element->setAttribute ("name", "Master");

            auto* channel = element->createNewChildElement ("Channel");
            channel->setAttribute ("audioChannels", 2);
            channel->setAttribute ("role", "master");
            channel->setAttribute ("solo", "false");
            channel->setAttribute ("id", channelID (0));
            writeDevices (*channel, master, "the master");
            addParameter (*channel, "Mute", "cmasterm", "Mute")->setAttribute ("value", "false");
            addPan (*channel, "cmasterp", (double) master.getProperty (schema::channelPan, 0.0));
            addVolume (*channel, "cmasterv", toLinear ((float) master.getProperty (schema::channelVolume, -3.0)));

            auto* lane = lanes->createNewChildElement ("Lanes");
            lane->setAttribute ("track", "tmaster");
            lane->setAttribute ("id", "lmaster");
            writeAutomation (*lane, master, channelID (0));

            if (lane->getNumChildElements() == 0)
                lanes->removeChildElement (lane, true);
        }

        //==============================================================================
        void writeLanes (const juce::ValueTree& track, std::uint64_t id, const juce::String& trackElementID)
        {
            auto* lane = lanes->createNewChildElement ("Lanes");
            lane->setAttribute ("track", trackElementID);
            lane->setAttribute ("id", "l" + juce::String ((juce::int64) id));
            auto* clips = lane->createNewChildElement ("Clips");
            clips->setAttribute ("id", "k" + juce::String ((juce::int64) id));

            for (const auto& clip : track)
            {
                if (! clip.hasType (schema::CLIP))
                    continue;

                if (clip[schema::clipKind].toString() == schema::clipIsAudio)
                    writeAudioClip (*clips, clip);
                else
                    writeMidiClip (*clips, clip);
            }

            writeAutomation (*lane, track, channelID (id));
        }

        void writeMidiClip (juce::XmlElement& clips, const juce::ValueTree& clip)
        {
            const auto id = clip[schema::clipID].toString();
            auto* element = clips.createNewChildElement ("Clip");
            element->setAttribute ("time", number ((double) clip[schema::clipStart]));
            element->setAttribute ("duration", number ((double) clip[schema::clipLength]));
            element->setAttribute ("playStart", number ((double) clip.getProperty (schema::clipOffset, 0.0)));
            element->setAttribute ("name", clip[schema::clipName].toString());

            if ((bool) clip[schema::clipMute])
                element->setAttribute ("enable", "false");

            auto* notes = element->createNewChildElement ("Notes");
            notes->setAttribute ("id", "n" + id);

            for (const auto& note : clip)
            {
                if (note.hasType (schema::CONTROL))
                    leftOut.addIfNotAlreadyThere ("clips' controller events");

                if (! note.hasType (schema::NOTE))
                    continue;

                if ((bool) note[schema::noteMute] || (bool) note[schema::noteSlide] || (bool) note[schema::notePortamento])
                    leftOut.addIfNotAlreadyThere ("notes' mute, slide and glide");

                if ((bool) note[schema::noteMute] || (bool) note[schema::noteSlide])
                    continue;

                auto* written = notes->createNewChildElement ("Note");
                written->setAttribute ("time", number ((double) note[schema::noteStart]));
                written->setAttribute ("duration", number ((double) note[schema::noteLength]));
                written->setAttribute ("channel", 0);
                written->setAttribute ("key", (int) note[schema::notePitch]);
                written->setAttribute ("vel", number ((int) note.getProperty (schema::noteVelocity, 100) / 127.0));
            }
        }

        void writeAudioClip (juce::XmlElement& clips, const juce::ValueTree& clip)
        {
            const auto path = clip[schema::clipFile].toString();
            const auto file =
                juce::File::isAbsolutePath (path) ? juce::File (path) : song.getFile().getParentDirectory().getChildFile (path);
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

            if (reader == nullptr)
            {
                leftOut.addIfNotAlreadyThere (file.getFileName() + ", which is missing");
                return;
            }

            const auto start = (double) clip[schema::clipStart];
            const auto from = (double) clip[schema::clipFileStart];
            const auto to = (double) clip.getProperty (schema::clipFileEnd, (double) reader->lengthInSamples / reader->sampleRate);
            const auto end = tempo.toBeats (tempo.toSamples (start) + (to - from));

            auto* element = clips.createNewChildElement ("Clip");
            element->setAttribute ("time", number (start));
            element->setAttribute ("duration", number (end - start));
            element->setAttribute ("contentTimeUnit", "seconds");
            element->setAttribute ("playStart", number (from));
            element->setAttribute ("playStop", number (to));
            element->setAttribute ("fadeTimeUnit", "seconds");
            element->setAttribute ("fadeInTime", number ((double) clip.getProperty (schema::clipFadeIn, 0.0)));
            element->setAttribute ("fadeOutTime", number ((double) clip.getProperty (schema::clipFadeOut, 0.0)));
            element->setAttribute ("name", clip[schema::clipName].toString());

            if ((bool) clip[schema::clipMute])
                element->setAttribute ("enable", "false");

            auto* audio = element->createNewChildElement ("Audio");
            audio->setAttribute ("channels", (int) reader->numChannels);
            audio->setAttribute ("sampleRate", (int) reader->sampleRate);
            audio->setAttribute ("duration", number ((double) reader->lengthInSamples / reader->sampleRate));
            audio->setAttribute ("id", "a" + clip[schema::clipID].toString());
            audio->createNewChildElement ("File")->setAttribute ("path", addAudioFile (file));
        }

        // A file's path in the zip: audio/ and its name, a number added where two files have one name.
        juce::String addAudioFile (const juce::File& file)
        {
            if (const auto found = audioFiles.find (file.getFullPathName()); found != audioFiles.end())
                return found->second;

            auto path = "audio/" + file.getFileName();

            for (int n = 2;
                 std::any_of (audioFiles.begin(), audioFiles.end(), [&path] (const auto& entry) { return entry.second == path; }); ++n)
                path = "audio/" + file.getFileNameWithoutExtension() + " " + juce::String (n) + file.getFileExtension();

            audioFiles[file.getFullPathName()] = path;
            content.files.emplace_back (path, file);
            return path;
        }

        // The automation of a channel's fader (its volume and pan) and of its VST3 plug-ins, as points on its lanes.
        void writeAutomation (juce::XmlElement& lane, const juce::ValueTree& track, const juce::String& channel)
        {
            const auto fader = readID (track[schema::channelFader]);
            std::set<std::uint64_t> pluginIDs;

            for (const auto& device : track)
                if (isVst3 (device))
                    pluginIDs.insert (readID (device[schema::deviceID]));

            for (const auto& curve : song.getState().getChildWithName (schema::AUTOMATION))
            {
                if (! curve.hasType (schema::CURVE))
                    continue;

                const auto device = readID (curve[schema::curveDevice]);
                const auto parameter = curve[schema::curveParameter].toString();
                juce::String target, unit;
                std::function<double (float)> toValue;

                if (device == fader && fader != 0 && (parameter == schema::faderVolume || parameter == schema::masterVolume))
                {
                    target = channel + "v", unit = "linear";
                    toValue = [] (float position) { return toLinear (faderPositionToDecibels (position)); };
                }
                else if (device == fader && fader != 0 && (parameter == schema::faderPan || parameter == schema::masterPan))
                {
                    target = channel + "p", unit = "normalized";
                    toValue = [] (float pan) { return (pan + 1.0) / 2.0; };
                }
                else if (pluginIDs.count (device) > 0)
                {
                    target = "d" + juce::String ((juce::int64) device) + "p" + parameter, unit = "normalized";
                    toValue = [] (float value) { return (double) value; };
                }
                else
                {
                    continue;
                }

                auto* points = lane.createNewChildElement ("Points");
                points->setAttribute ("unit", unit);
                points->setAttribute ("id", "q" + target);
                points->createNewChildElement ("Target")->setAttribute ("parameter", target);

                for (const auto& point : curve)
                {
                    if (! point.hasType (schema::POINT))
                        continue;

                    if (! juce::approximatelyEqual ((float) point[schema::pointCurve], 0.0f))
                        leftOut.addIfNotAlreadyThere ("automation's bends");

                    auto* element = points->createNewChildElement ("RealPoint");
                    element->setAttribute ("time", number ((double) point[schema::pointBeat]));
                    element->setAttribute ("value", number (toValue ((float) point[schema::pointValue])));
                    element->setAttribute ("interpolation", "linear");
                }
            }
        }
    };
} // namespace

DawProjectContent describeDawProject (const Song& song, PluginInstances* plugins, const juce::String& title, const juce::String& comment)
{
    JUCE_ASSERT_MESSAGE_THREAD
    DawProjectContent content;
    Writer writer { song, plugins, content };
    writer.write (title, comment);
    return content;
}

juce::Result writeDawProject (DawProjectContent& content, const juce::File& bouncesFolder, const juce::File& file)
{
    // Each bounce's clip as long as its file: its length in seconds, and in beats from the song's start.
    for (const auto& [track, clip] : content.bounces)
    {
        const auto bounce = bouncesFolder.getChildFile (juce::String ((juce::int64) track.getRaw()) + ".wav");
        juce::WavAudioFormat wav;
        const std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (bounce), true));

        if (reader == nullptr)
            return juce::Result::fail ("Phazite couldn't read the bounce of a track.");

        const auto seconds = (double) reader->lengthInSamples / reader->sampleRate;
        clip->setAttribute ("duration", number (content.tempo.toBeats (seconds)));
        clip->setAttribute ("playStop", number (seconds));

        if (auto* audio = clip->getChildByName ("Audio"))
            audio->setAttribute ("duration", number (seconds));

        content.files.emplace_back ("bounces/" + bounce.getFileName(), bounce);
    }

    // project.xml and metadata.xml, the files (stored: audio barely compresses) and the presets.
    const auto now = juce::Time::getCurrentTime();
    juce::ZipFile::Builder zip;

    for (const auto& [xml, path] :
         { std::pair { content.project.get(), "project.xml" }, std::pair { content.metadata.get(), "metadata.xml" } })
    {
        const auto text = xml->toString();
        zip.addEntry (std::make_unique<juce::MemoryInputStream> (text.toRawUTF8(), text.getNumBytesAsUTF8(), true), 9, path, now);
    }

    for (const auto& [path, source] : content.files)
        zip.addFile (source, 0, path);

    for (const auto& [path, blob] : content.blobs)
        zip.addEntry (std::make_unique<juce::MemoryInputStream> (blob, true), 9, path, now);

    // Into a temporary file next to it, which takes its place once complete.
    const auto failed = juce::Result::fail ("Phazite couldn't write " + file.getFileName() + ".");
    file.getParentDirectory().createDirectory();
    juce::TemporaryFile temporary (file);

    {
        juce::FileOutputStream out (temporary.getFile());

        if (! out.openedOk() || ! zip.writeToStream (out, nullptr))
            return failed;
    }

    if (! temporary.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Phazite couldn't replace " + file.getFileName() + ". Is it open in another app?");

    return juce::Result::ok();
}

//==============================================================================
namespace
{
    // A song exported as a DAWproject: its bounces rendered into a temporary folder, then the zip written, on the render's
    // thread; what came of it told on the message thread.
    class DawProjectExport final : public TrackRender::Impl
    {
    public:
        DawProjectExport (Song& s, juce::File f, juce::String t, std::function<void (const juce::Result&)> callback)
            : song (s),
              file (std::move (f)),
              title (std::move (t)),
              onFinished (std::move (callback)),
              alive (std::make_shared<DawProjectExport*> (this))
        {
        }

        ~DawProjectExport() override
        {
            alive = nullptr;  // no call back from here on
            render = nullptr; // cancels what is left, and waits for its thread
            ownPlugins = nullptr;
            bouncesFolder.deleteRecursively();
        }

        void start (PluginHost* pluginHost, const juce::String& comment)
        {
            JUCE_ASSERT_MESSAGE_THREAD
            auto* session = findSession (song);
            PluginInstances* instances = session != nullptr ? &session->getPlugins() : nullptr;

            // A song no session plays (an old version's): its plug-ins in instances of its own, in the background.
            if (session == nullptr && pluginHost != nullptr)
            {
                ownPlugins = std::make_unique<PluginInstances> (song, *pluginHost, bounceRate);
                instances = ownPlugins.get();
            }

            content = describeDawProject (song, instances, title, comment);
            std::vector<OfflineRender::Job> jobs;
            const auto end = toSeconds (song, getSongEnd (song));

            for (const auto& [track, clip] : content.bounces)
                jobs.push_back ({ bouncesFolder.getChildFile (juce::String ((juce::int64) track.getRaw()) + ".wav"), end, tailSeconds,
                                  SongPlayer::RenderOf { (std::int64_t) track.getRaw(), true, std::nullopt } });

            if (! jobs.empty())
            {
                bouncesFolder.createDirectory();

                if (session != nullptr && isSongPlaying (song))
                    stopAndReturn (song);

                renderMissingClipAudio (song); // the engine plays a clip whose render isn't there silent
            }

            const OfflineRender::Setup setup { .session = jobs.empty() ? nullptr : session,
                                               .makePlayer = [this, instances] { return std::make_unique<SongPlayer> (song, instances); },
                                               .numThreads = session != nullptr ? session->getHost().getNumThreads() : 1,
                                               .sampleRate = bounceRate,
                                               .blockSize = bounceBlockSize,
                                               .background = session == nullptr };

            render =
                std::make_unique<OfflineRender> (setup, jobs, OfflineRender::Rendered(),
                                                 [this, weakAlive = std::weak_ptr<DawProjectExport*> (alive)] (const juce::Result& rendered)
                                                 {
                                                     // On the render's thread, once the bounces are: the zip.
                                                     const auto result =
                                                         rendered.wasOk() ? writeDawProject (content, bouncesFolder, file) : rendered;
                                                     juce::MessageManager::callAsync (
                                                         [weakAlive, result]
                                                         {
                                                             if (const auto locked = weakAlive.lock())
                                                                 (*locked)->finish (result);
                                                         });
                                                 });
        }

        float getProgress() const override { return render != nullptr ? render->getProgress() : 0.0f; }
        juce::String getTitle() const override { return "Exporting " + title + " as DAWproject"; }

    private:
        Song& song;
        const juce::File file;
        const juce::String title;
        const std::function<void (const juce::Result&)> onFinished;
        std::shared_ptr<DawProjectExport*> alive;
        const juce::File bouncesFolder =
            juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("Phazite DAWproject", {}, false);
        DawProjectContent content;
        std::unique_ptr<PluginInstances> ownPlugins; // goes after the render that plays them
        std::unique_ptr<OfflineRender> render;

        void finish (const juce::Result& result)
        {
            render = nullptr; // the project plays its plug-ins again
            ownPlugins = nullptr;

            if (const auto callback = onFinished; callback != nullptr)
                callback (result); // last: it may delete this
        }
    };
} // namespace

std::unique_ptr<TrackRender> startDawProjectExport (Song& song, PluginHost* pluginHost, const juce::File& file, const juce::String& title,
                                                    const juce::String& comment, std::function<void (const juce::Result&)> onFinished)
{
    auto exporting = std::make_unique<DawProjectExport> (song, file, title, std::move (onFinished));
    exporting->start (pluginHost, comment);
    return std::make_unique<TrackRender> (std::move (exporting));
}

//==============================================================================
DawProjectFile readDawProject (const juce::File& file)
{
    DawProjectFile read;
    read.file = file;
    juce::ZipFile zip (file);

    const auto parse = [&zip] (const char* name) -> std::unique_ptr<juce::XmlElement>
    {
        const auto index = zip.getIndexOfFileName (name);
        const std::unique_ptr<juce::InputStream> stream (index >= 0 ? zip.createStreamForEntry (index) : nullptr);
        return stream != nullptr ? juce::parseXML (stream->readEntireStreamAsString()) : nullptr;
    };

    read.project = parse ("project.xml");
    read.metadata = parse ("metadata.xml");

    if (read.project == nullptr || ! read.project->hasTagName ("Project"))
    {
        read.error = file.getFileName() + " isn't a DAWproject Phazite can read.";
        read.project = nullptr;
        return read;
    }

    // The version it came from, as Phazite's comment names it: the 64 hexadecimal digits of its id, in brackets.
    if (const auto* comment = read.metadata != nullptr ? read.metadata->getChildByName ("Comment") : nullptr)
    {
        const auto text = comment->getAllSubText();

        for (auto open = text.indexOfChar ('('); open >= 0; open = text.indexOfChar (open + 1, '('))
            if (const auto id = text.substring (open + 1, open + 65);
                id.length() == 64 && id.containsOnly ("0123456789abcdef") && text[open + 65] == ')')
                read.fromVersion = id;
    }

    return read;
}

namespace
{
    // How close two of a DAWproject's numbers must be to be the same: other DAWs write fewer decimals than Phazite does.
    bool isClose (double a, double b, double within = 1.0e-5)
    {
        return std::abs (a - b) <= within * std::max (1.0, std::abs (b));
    }

    // A DAWproject's tracks, by their elements' ids and in order: those that are the song's (not the master, nor a bounce
    // Phazite added, nor a folder, whose tracks are among them), and their lanes.
    struct ReadTracks
    {
        std::vector<const juce::XmlElement*> tracks;
        std::map<juce::String, const juce::XmlElement*> byID, lanes;
        const juce::XmlElement* master = nullptr;

        explicit ReadTracks (const juce::XmlElement& project)
        {
            if (const auto* structure = project.getChildByName ("Structure"))
                collect (*structure);

            if (const auto* arrangement = project.getChildByName ("Arrangement"))
                if (const auto* all = arrangement->getChildByName ("Lanes"))
                    for (const auto* lane : all->getChildWithTagNameIterator ("Lanes"))
                        lanes[lane->getStringAttribute ("track")] = lane;
        }

        void collect (const juce::XmlElement& parent)
        {
            for (const auto* track : parent.getChildWithTagNameIterator ("Track"))
            {
                const auto* channel = track->getChildByName ("Channel");

                if (channel != nullptr && channel->getStringAttribute ("role") == "master")
                    master = track;
                else if (track->getStringAttribute ("contentType").contains ("tracks"))
                    collect (*track);
                else if (! isBounce (*track))
                    tracks.push_back (track);

                byID[track->getStringAttribute ("id")] = track;
            }
        }

        static bool isBounce (const juce::XmlElement& track) { return track.getStringAttribute ("name").endsWith (" (bounce)"); }

        // A lane's clips, in order: those with notes, or those with audio.
        std::vector<const juce::XmlElement*> getClips (const juce::XmlElement& track, bool withNotes) const
        {
            std::vector<const juce::XmlElement*> clips;
            const auto lane = lanes.find (track.getStringAttribute ("id"));

            if (lane != lanes.end())
                for (const auto* group : lane->second->getChildWithTagNameIterator ("Clips"))
                    for (const auto* clip : group->getChildWithTagNameIterator ("Clip"))
                        if ((clip->getChildByName ("Notes") != nullptr) == withNotes
                            && (withNotes || clip->getChildByName ("Audio") != nullptr))
                            clips.push_back (clip);

            return clips;
        }
    };

    double readValue (const juce::XmlElement* channel, const char* parameter, double otherwise)
    {
        const auto* element = channel != nullptr ? channel->getChildByName (parameter) : nullptr;
        return element != nullptr ? element->getDoubleAttribute ("value", otherwise) : otherwise;
    }

    bool readMute (const juce::XmlElement* channel)
    {
        const auto* element = channel != nullptr ? channel->getChildByName ("Mute") : nullptr;
        return element != nullptr && element->getStringAttribute ("value") == "true";
    }

    float toDecibels (double linear)
    {
        return linear <= 0.0 ? -100.0f : (float) (20.0 * std::log10 (linear));
    }

    struct Reader
    {
        Song& song;
        const DawProjectFile& in;
        juce::StringArray notTaken;

        void take()
        {
            const auto written = describeDawProject (song, nullptr, {}, {});
            const ReadTracks theirs (*in.project), ours (*written.project);
            const auto isNew = getTrackIDs (song).size() <= 1 && getClipCount() == 0;
            std::set<const juce::XmlElement*> matched;
            std::vector<TrackID> gone;

            // Each of the song's tracks, matched by Phazite's id or by its name; a new song's own, none: its tracks are the file's.
            for (const auto* track : ours.tracks)
            {
                const auto id = TrackID::fromRaw ((std::uint64_t) track->getStringAttribute ("id").substring (1).getLargeIntValue());
                const juce::XmlElement* found = nullptr;

                if (isNew)
                {
                    gone.push_back (id);
                    continue;
                }

                if (const auto byID = theirs.byID.find (track->getStringAttribute ("id"));
                    byID != theirs.byID.end() && ! ReadTracks::isBounce (*byID->second))
                    found = byID->second;

                for (const auto* other : theirs.tracks)
                    if (found == nullptr && matched.count (other) == 0
                        && other->getStringAttribute ("name") == track->getStringAttribute ("name"))
                        found = other;

                if (found == nullptr || matched.count (found) > 0)
                {
                    gone.push_back (id);
                    continue;
                }

                matched.insert (found);
                takeChannel (id, *found, *track);
                takeNotes (id, theirs.getClips (*found, true));
            }

            if (! gone.empty())
                deleteTracks (song, gone, nullptr);

            // The tracks it adds.
            for (const auto* track : theirs.tracks)
                if (matched.count (track) == 0)
                    addTrack (*track, theirs);

            // The master's fader; and into a new song, the tempo, the time signature and the markers.
            if (theirs.master != nullptr && ours.master != nullptr)
                takeFader ({}, theirs.master->getChildByName ("Channel"), ours.master->getChildByName ("Channel"));

            if (isNew)
                takeTransport();
        }

        int getClipCount() const
        {
            auto count = 0;

            for (const auto& track : getTrackIDs (song))
                for (const auto& child : findTrackState (song, track))
                    count += child.hasType (schema::CLIP) ? 1 : 0;

            return count;
        }

        void takeFader (TrackID id, const juce::XmlElement* theirs, const juce::XmlElement* ours)
        {
            if (const auto volume = readValue (theirs, "Volume", 1.0); ! isClose (volume, readValue (ours, "Volume", 1.0)))
                setChannelVolume (song, id, toDecibels (volume));

            if (const auto pan = readValue (theirs, "Pan", 0.5); ! isClose (pan, readValue (ours, "Pan", 0.5)))
                setChannelPan (song, id, (float) juce::jlimit (-1.0, 1.0, pan * 2.0 - 1.0));
        }

        void takeChannel (TrackID id, const juce::XmlElement& theirs, const juce::XmlElement& ours)
        {
            const auto* theirChannel = theirs.getChildByName ("Channel");
            const auto* ourChannel = ours.getChildByName ("Channel");
            takeFader (id, theirChannel, ourChannel);

            if (const auto mute = readMute (theirChannel); mute != readMute (ourChannel))
                setTrackMuted (song, id, mute);
        }

        // Its MIDI clips' notes, clip by clip in order: those that are a note the clip has (within what the numbers keep)
        // stay as they are; the others come, and the clip's notes it hasn't go (but those a DAWproject has no place for).
        void takeNotes (TrackID id, const std::vector<const juce::XmlElement*>& clips)
        {
            std::vector<juce::ValueTree> ourClips;

            for (const auto& child : findTrackState (song, id))
                if (child.hasType (schema::CLIP) && child[schema::clipKind].toString() != schema::clipIsAudio)
                    ourClips.push_back (child);

            if (ourClips.size() != clips.size())
                notTaken.addIfNotAlreadyThere ("clips added or taken away in a track the song has");

            for (size_t i = 0; i < std::min (ourClips.size(), clips.size()); ++i)
            {
                auto clip = ourClips[i];
                std::vector<juce::ValueTree> unused;

                for (const auto& note : clip)
                    if (note.hasType (schema::NOTE) && ! (bool) note[schema::noteMute] && ! (bool) note[schema::noteSlide])
                        unused.push_back (note);

                std::vector<const juce::XmlElement*> added;

                for (const auto* note : clips[i]->getChildByName ("Notes")->getChildWithTagNameIterator ("Note"))
                {
                    const auto same =
                        std::find_if (unused.begin(), unused.end(),
                                      [note] (const juce::ValueTree& ours)
                                      {
                                          return (int) ours[schema::notePitch] == note->getIntAttribute ("key")
                                                 && isClose ((double) ours[schema::noteStart], note->getDoubleAttribute ("time"))
                                                 && isClose ((double) ours[schema::noteLength], note->getDoubleAttribute ("duration"))
                                                 && std::abs ((int) ours.getProperty (schema::noteVelocity, 100)
                                                              - juce::roundToInt (note->getDoubleAttribute ("vel", 0.8) * 127.0))
                                                        <= 1;
                                      });

                    if (same != unused.end())
                        unused.erase (same);
                    else
                        added.push_back (note);
                }

                for (const auto& note : unused)
                    clip.removeChild (note, nullptr);

                const auto clipID = getClipID (clip);
                const auto contentStart = getContentStart (clip);

                for (const auto* note : added)
                    addNoteFrom (clipID, contentStart, *note);
            }
        }

        void addNoteFrom (ClipID clip, BeatPosition contentStart, const juce::XmlElement& note)
        {
            addNote (song, clip, juce::jlimit (0, 127, note.getIntAttribute ("key")),
                     contentStart + BeatDuration::fromBeats (note.getDoubleAttribute ("time")),
                     BeatDuration::fromBeats (std::max (1.0 / 64.0, note.getDoubleAttribute ("duration"))),
                     juce::jlimit (1, 127, juce::roundToInt (note.getDoubleAttribute ("vel", 0.8) * 127.0)));
        }

        void addTrack (const juce::XmlElement& track, const ReadTracks& theirs)
        {
            const auto tracks = getTrackIDs (song);
            const auto id = insertTrack (song, tracks.empty() ? TrackID() : tracks.back(), nullptr);

            if (! id.isValid())
                return;

            setTrackName (song, id, track.getStringAttribute ("name"));

            if (const auto colour = juce::Colour::fromString ("ff" + track.getStringAttribute ("color").trimCharactersAtStart ("#"));
                track.hasAttribute ("color") && ! colour.isTransparent())
                setTrackColour (song, id, colour);

            const auto* channel = track.getChildByName ("Channel");
            setChannelVolume (song, id, toDecibels (readValue (channel, "Volume", 1.0)));
            setChannelPan (song, id, (float) juce::jlimit (-1.0, 1.0, readValue (channel, "Pan", 0.5) * 2.0 - 1.0));
            setTrackMuted (song, id, readMute (channel));

            if (channel != nullptr && channel->getChildByName ("Sends") != nullptr)
                notTaken.addIfNotAlreadyThere ("sends");

            if (channel != nullptr && channel->getChildByName ("Devices") != nullptr)
                notTaken.addIfNotAlreadyThere ("plug-ins");

            for (const auto* clip : theirs.getClips (track, true))
            {
                const auto start = clip->getDoubleAttribute ("time");
                const auto clipID = placeClipState (
                    song,
                    makeMidiClipState (song, clip->getStringAttribute ("name"),
                                       { BeatPosition::fromBeats (start),
                                         BeatPosition::fromBeats (start + std::max (0.25, clip->getDoubleAttribute ("duration"))) }),
                    id);

                if (! clipID.isValid())
                    continue;

                auto clipState = findClipState (song, clipID);
                clipState.setProperty (schema::clipOffset, std::max (0.0, clip->getDoubleAttribute ("playStart")), nullptr);

                for (const auto* note : clip->getChildByName ("Notes")->getChildWithTagNameIterator ("Note"))
                    addNoteFrom (clipID, getContentStart (clipState), *note);
            }

            for (const auto* clip : theirs.getClips (track, false))
                addAudioClip (id, *clip);
        }

        // An audio clip: its file copied out of the zip into the song's Audio folder, the part of it the clip plays, its fades.
        void addAudioClip (TrackID track, const juce::XmlElement& clip)
        {
            const auto* audio = clip.getChildByName ("Audio");
            const auto* file = audio != nullptr ? audio->getChildByName ("File") : nullptr;

            if (file == nullptr || clip.getStringAttribute ("contentTimeUnit", "beats") != "seconds")
            {
                notTaken.addIfNotAlreadyThere ("audio clips that follow the tempo");
                return;
            }

            const auto copied = copyOut (file->getStringAttribute ("path"));

            if (copied == juce::File())
            {
                notTaken.addIfNotAlreadyThere ("audio files that aren't in it");
                return;
            }

            const auto from = clip.getDoubleAttribute ("playStart");
            const auto to = clip.getDoubleAttribute ("playStop", audio->getDoubleAttribute ("duration"));
            auto state =
                makeAudioClipState (song, clip.getStringAttribute ("name", copied.getFileNameWithoutExtension()), copied,
                                    BeatPosition::fromBeats (clip.getDoubleAttribute ("time")), { from, std::max (from + 0.001, to) });

            if (clip.getStringAttribute ("fadeTimeUnit", "seconds") == "seconds")
            {
                state.setProperty (schema::clipFadeIn, clip.getDoubleAttribute ("fadeInTime"), nullptr);
                state.setProperty (schema::clipFadeOut, clip.getDoubleAttribute ("fadeOutTime"), nullptr);
            }

            placeClipState (song, state, track);
        }

        juce::File copyOut (const juce::String& path)
        {
            juce::ZipFile zip (in.file);
            const auto index = zip.getIndexOfFileName (path);
            const auto folder = song.getFile().getSiblingFile ("Audio");

            if (index < 0 || song.getFile() == juce::File())
                return {};

            const auto target =
                folder.getNonexistentChildFile (juce::File::createLegalFileName (juce::File (path).getFileNameWithoutExtension()),
                                                juce::File (path).getFileExtension(), false);
            const std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (index));
            folder.createDirectory();

            if (stream == nullptr)
                return {};

            juce::FileOutputStream out (target);

            if (! out.openedOk() || out.writeFromInputStream (*stream, -1) < 0)
                return {};

            return target;
        }

        void takeTransport()
        {
            if (const auto* transport = in.project->getChildByName ("Transport"))
            {
                if (const auto* tempo = transport->getChildByName ("Tempo"))
                    setTempo (song, tempo->getDoubleAttribute ("value", 120.0));

                if (const auto* signature = transport->getChildByName ("TimeSignature"))
                    setTimeSignature (song, { signature->getIntAttribute ("numerator", 4), signature->getIntAttribute ("denominator", 4) });
            }

            const auto* arrangement = in.project->getChildByName ("Arrangement");
            const auto* markers = arrangement != nullptr ? arrangement->getChildByName ("Markers") : nullptr;
            std::vector<const juce::XmlElement*> all;

            if (markers != nullptr)
                for (const auto* marker : markers->getChildWithTagNameIterator ("Marker"))
                    all.push_back (marker);

            // Each a section, up to the next.
            for (size_t i = 0; i < all.size(); ++i)
            {
                const auto start = all[i]->getDoubleAttribute ("time");
                const auto end = i + 1 < all.size() ? all[i + 1]->getDoubleAttribute ("time") : start + 32.0;
                SectionInfo section;
                section.name = all[i]->getStringAttribute ("name");
                section.length = BeatDuration::fromBeats (std::max (1.0, end - start));
                insertSection (song, BeatPosition::fromBeats (start), section);
            }
        }
    };
} // namespace

juce::StringArray takeDawProject (Song& song, const DawProjectFile& in)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (in.project == nullptr)
        return { in.error };

    Reader reader { song, in };
    reader.take();
    return reader.notTaken;
}

bool importDawProjectAsBranch (ProjectContext& context, const DawProjectFile& in,
                               const std::function<void (const juce::ValueTree& song)>& openSong, juce::StringArray& said)
{
    auto* history = getVersionHistory (context);
    auto* open = findSong (context);
    const auto* version = history != nullptr ? history->findVersion (in.fromVersion) : nullptr;

    if (open == nullptr || version == nullptr || in.project == nullptr)
    {
        said = { "It wasn't exported from a version of this project." };
        return false;
    }

    // The version's song, made a song of its own, with what came back taken in; then a branch from the version, with it.
    const auto state = history->readSong (version->id);
    const auto song = state.isValid() ? makeSong (state, open->getFile(), open->getCacheFolder()) : nullptr;

    if (song == nullptr)
    {
        said = { "Phazite couldn't read the version it came from." };
        return false;
    }

    said = takeDawProject (*song, in);
    const auto branch = history->makeBranchWithSong (version->id, song->getState());

    if (branch.isEmpty() || ! switchBranch (context, branch, openSong))
    {
        said = { "Phazite couldn't make a branch for it." };
        return false;
    }

    return true;
}
} // namespace phazite
