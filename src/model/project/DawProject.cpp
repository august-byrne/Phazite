// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DawProject.h"
#include "Export.h"
#include "engine/automation/Curve.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Json.h"
#include "model/base/Numbers.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/WholeState.h"
#include "model/engine/AppInfo.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/OfflineRender.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "sandbox/core/Vst3State.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <vector>

namespace phazite
{
namespace
{
    constexpr double bounceRate = 48000.0;
    constexpr int bounceBlockSize = 512;
    constexpr int minimumTempoPieces = 16; // straight pieces a bent tempo line is written as, at least

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

    // How far another DAW's straight lines between the points written may be from the line Phazite plays: in decibels for a
    // linear gain (a fader's volume, a send's level), a level below automationFloorDb counting as that, where nothing is
    // heard; and as a part of the range for a normalized value (a pan, a plug-in's parameter). A line is halved at most so
    // many times: 4096 pieces.
    constexpr double automationToleranceDb = 0.1, automationFloorDb = -80.0, automationToleranceNormalized = 0.001;
    constexpr int maximumHalvings = 12;
    constexpr int probesPerPiece = 8; // the places in each piece its straight line is compared at

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

    // What of a track another DAW can't play, which its bounce plays: why, or nothing. Its modulation (Modulation.h) and its
    // clips read by their IDs (Clips.h), after its other parts.
    juce::String findWhyBounced (const Song& song, const juce::ValueTree& track)
    {
        if ((bool) track[schema::trackFrozen])
            return "frozen";

        for (const auto& child : track)
            if (child.hasType (schema::CONTAINER) || (child.hasType (schema::DEVICE) && ! isVst3 (child)))
                return "devices";

        if (hasModulations (song, getTrackID (track)))
            return "modulation";

        // An audio clip's gain, tempo (while it follows the song's), pitch, direction and takes, as only Phazite plays them.
        for (const auto& clip : getClipValues (song, getTrackID (track)))
            if (clip.isAudio()
                && (! juce::approximatelyEqual (clip.gain, 0.0f) || clip.sound.tempo > 0.0
                    || ! juce::approximatelyEqual (clip.sound.pitch, 0.0f) || clip.sound.reversed || clip.takes != 0))
                return "audio clips";

        return {};
    }

    //==============================================================================
    struct Writer
    {
        const Song& song;
        DawProjectContent& content;
        engine::TempoMap tempo { getTempoMap (song) };      // in seconds
        juce::StringArray leftOut {};                       // what couldn't go in
        std::map<juce::String, juce::String> audioFiles {}; // each file's full path: its path in the zip
        juce::XmlElement* lanes = nullptr;                  // the arrangement's

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
                points->setAttribute ("timeUnit", "beats");
                points->createNewChildElement ("Target")->setAttribute ("parameter", "tempo");

                const auto addPoint = [points] (double beat, double bpm, bool linear)
                {
                    auto* point = points->createNewChildElement ("RealPoint");
                    point->setAttribute ("time", number (beat));
                    point->setAttribute ("value", number (bpm));
                    point->setAttribute ("interpolation", linear ? "linear" : "hold");
                };

                // A DAWproject's tempo holds or ramps straight from point to point: a bent line goes as straight pieces along
                // it, a beat long at most, and one that jumps at once holds the next tempo from its start.
                for (size_t i = 0; i < tempos.size(); ++i)
                {
                    const auto& change = tempos[i];
                    const auto from = change.beat.inBeats();
                    const auto curve = juce::jlimit (-1.0f, 1.0f, change.curve);

                    if (i + 1 >= tempos.size() || juce::exactlyEqual (curve, tempoJump))
                    {
                        addPoint (from, change.bpm, false);
                    }
                    else if (juce::exactlyEqual (curve, -1.0f))
                    {
                        addPoint (from, tempos[i + 1].bpm, false);
                    }
                    else if (juce::exactlyEqual (curve, tempoRamp))
                    {
                        addPoint (from, change.bpm, true);
                    }
                    else
                    {
                        const auto to = tempos[i + 1].beat.inBeats();
                        const auto pieces = std::max (minimumTempoPieces, (int) std::ceil (to - from));

                        for (int piece = 0; piece < pieces; ++piece)
                        {
                            const auto beat = from + (to - from) * piece / pieces;
                            addPoint (beat, piece == 0 ? change.bpm : tempo.getTempoAt (beat), true);
                        }
                    }
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
            // The song's sections as their value holds them (SectionsValue.h), by their starts; a colour of their own alone.
            const auto value = getSectionsValue (song);
            std::vector<SectionValue> sections (value.sections.begin(), value.sections.end());

            if (sections.empty())
                return;

            std::stable_sort (sections.begin(), sections.end(),
                              [] (const SectionValue& a, const SectionValue& b) { return a.start < b.start; });
            auto* markers = arrangement.createNewChildElement ("Markers");
            markers->setAttribute ("id", "markers");

            for (const auto& section : sections)
            {
                auto* marker = markers->createNewChildElement ("Marker");
                marker->setAttribute ("time", number (section.start.inBeats()));
                marker->setAttribute ("name", section.name);

                if (const auto colour = juce::Colour::fromString (section.colour.value_or (juce::String())); ! colour.isTransparent())
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
            const auto why = isFolder ? juce::String() : findWhyBounced (song, track);

            auto* element = parent.createNewChildElement ("Track");
            element->setAttribute ("contentType", isFolder                                           ? "tracks"
                                                  : hasClips (song, trackID, ClipValue::Kind::audio) ? "audio"
                                                                                                     : "notes");
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
            const auto preset = takePreset (device);

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

            for (const auto& curve : getCurveKeys (song))
            {
                if (curve.device.getRaw() != id)
                    continue;

                if (parameters == nullptr)
                    parameters = plugin->createNewChildElement ("Parameters");

                auto* parameter = addParameter (*parameters, "RealParameter", elementID + "p" + curve.parameter, curve.parameter);
                parameter->setAttribute ("parameterID", curve.parameter.getIntValue());
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

        // Its state as the song keeps it, a VST3's preset file (sandbox/core/Vst3State.h): what DAWproject holds of a VST3
        // too. None for a state that isn't one (it never loaded here).
        static juce::MemoryBlock takePreset (const juce::ValueTree& device)
        {
            if (auto bytes = readBytes (device[schema::pluginState].toString());
                bytes.has_value() && sandbox::readVst3Preset (*bytes).has_value())
                return std::move (*bytes);

            return {};
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

            // Its clips by their IDs (Clips.h), in the order the track holds them.
            for (const auto& clip : getClipValues (song, TrackID::fromRaw (id)))
            {
                if (clip.isAudio())
                    writeAudioClip (*clips, clip);
                else
                    writeMidiClip (*clips, clip);
            }

            writeAutomation (*lane, track, channelID (id));
        }

        void writeMidiClip (juce::XmlElement& clips, const ClipValue& clip)
        {
            const auto id = juce::String ((juce::int64) clip.id.getRaw());
            auto* element = clips.createNewChildElement ("Clip");
            element->setAttribute ("time", number (clip.start.inBeats()));
            element->setAttribute ("duration", number (clip.length.inBeats()));
            element->setAttribute ("playStart", number (clip.offset.inBeats()));
            element->setAttribute ("name", getClipName (song, clip));

            if (clip.muted)
                element->setAttribute ("enable", "false");

            auto* notes = element->createNewChildElement ("Notes");
            notes->setAttribute ("id", "n" + id);

            // Its notes as the song holds them (Notes.h), each where it is among the clip's notes, as DAWproject counts them.
            const auto clipID = clip.id;
            const auto contentStart = getContentStart (clip);

            for (const auto& note : getClipNotes (song, clipID))
            {
                if (note.muted || note.slide || note.portamento)
                    leftOut.addIfNotAlreadyThere ("notes' mute, slide and glide");

                if (note.muted || note.slide)
                    continue;

                auto* written = notes->createNewChildElement ("Note");
                written->setAttribute ("time", number ((note.start - contentStart).inBeats()));
                written->setAttribute ("duration", number (note.length.inBeats()));
                written->setAttribute ("channel", note.channel - 1); // counted from 0 there
                written->setAttribute ("key", note.pitch);
                written->setAttribute ("vel", number (note.velocity / 127.0));
            }

            if (! getClipControls (song, clipID).empty())
                leftOut.addIfNotAlreadyThere ("clips' controller events");
        }

        void writeAudioClip (juce::XmlElement& clips, const ClipValue& clip)
        {
            const auto& path = clip.sound.file;
            const auto file =
                juce::File::isAbsolutePath (path) ? makeFileOfFullPath (path) : song.getFile().getParentDirectory().getChildFile (path);
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

            if (reader == nullptr)
            {
                leftOut.addIfNotAlreadyThere (file.getFileName() + ", which is missing");
                return;
            }

            const auto start = clip.start.inBeats();
            const auto from = clip.fileStart;
            // Where its part ends, as Phazite plays it: one whose end is 0 (an empty file's, a part set to nothing, or a damaged
            // file's clip without one, which the song holds as its description's 0: ClipsValue.h, readClipValue()) plays nothing.
            const auto to = clip.fileEnd;
            const auto end = tempo.toBeats (tempo.toSamples (start) + (to - from));

            auto* element = clips.createNewChildElement ("Clip");
            element->setAttribute ("time", number (start));
            element->setAttribute ("duration", number (end - start));
            element->setAttribute ("contentTimeUnit", "seconds");
            element->setAttribute ("playStart", number (from));
            element->setAttribute ("playStop", number (to));
            element->setAttribute ("fadeTimeUnit", "seconds");
            element->setAttribute ("fadeInTime", number (clip.fadeIn.seconds));
            element->setAttribute ("fadeOutTime", number (clip.fadeOut.seconds));
            element->setAttribute ("name", getClipName (song, clip));

            if (clip.muted)
                element->setAttribute ("enable", "false");

            auto* audio = element->createNewChildElement ("Audio");
            audio->setAttribute ("channels", (int) reader->numChannels);
            audio->setAttribute ("sampleRate", toWhole<int> (reader->sampleRate)); // a file's header says it
            audio->setAttribute ("duration", number ((double) reader->lengthInSamples / reader->sampleRate));
            audio->setAttribute ("id", "a" + juce::String ((juce::int64) clip.id.getRaw()));
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

        // The automation of a channel's fader (its volume and pan), of its sends' levels and of its VST3 plug-ins, as points on
        // its lanes.
        void writeAutomation (juce::XmlElement& lane, const juce::ValueTree& track, const juce::String& channel)
        {
            const auto fader = readID (track[schema::channelFader]);
            std::set<std::uint64_t> pluginIDs, sendIDs;

            for (const auto& child : track)
            {
                if (isVst3 (child))
                    pluginIDs.insert (readID (child[schema::deviceID]));
                else if (child.hasType (schema::SEND))
                    sendIDs.insert (readID (child[schema::sendID]));
            }

            const auto toGain = [] (float position) { return toLinear (faderPositionToDecibels (position)); };
            const auto toNormalized = [] (float position) { return (double) position; };

            // The song's curves in their order, each read as a value (Automation.h) for a channel it is of.
            for (const auto& curve : getCurveKeys (song))
            {
                const auto device = curve.device.getRaw();
                const auto& parameter = curve.parameter;
                juce::String target, unit;
                std::function<double (float)> toValue;

                if (device == fader && fader != 0 && (parameter == schema::faderVolume || parameter == schema::masterVolume))
                {
                    target = channel + "v", unit = "linear";
                    toValue = toGain;
                }
                else if (device == fader && fader != 0 && (parameter == schema::faderPan || parameter == schema::masterPan))
                {
                    target = channel + "p", unit = "normalized"; // a pan's position, as DAWproject's
                    toValue = toNormalized;
                }
                else if (sendIDs.count (device) > 0 && parameter == schema::sendLevelParameter)
                {
                    target = channel + "s" + juce::String ((juce::int64) device) + "v", unit = "linear"; // writeSends()' Volume
                    toValue = toGain;
                }
                else if (pluginIDs.count (device) > 0)
                {
                    target = "d" + juce::String ((juce::int64) device) + "p" + parameter, unit = "normalized";
                    toValue = toNormalized;
                }
                else
                {
                    continue;
                }

                auto* points = lane.createNewChildElement ("Points");
                points->setAttribute ("unit", unit);
                points->setAttribute ("id", "q" + target);
                points->createNewChildElement ("Target")->setAttribute ("parameter", target);
                writePoints (*points, getCurveValue (song, curve).value_or (CurveValue()), toValue, unit == "linear");
            }
        }

        // A curve's points as another DAW plays them: each with the value it has there, a straight line to the next in the
        // lane's unit (or held, a step at the next), and as many more along each of Phazite's lines as keep those straight
        // lines close to what Phazite plays there (engine/automation/Curve.h; the automation tolerances, above). Phazite's
        // line is straight along the control's travel: on a fader's travel that is no straight line in linear gain (known bug
        // 193: another DAW played other levels between the points), and a bent line is bent in any unit. Two points at one
        // beat are a jump, written so; a line bent all the way is a hold and a jump, at its end (held) or its start.
        static void writePoints (juce::XmlElement& points, const CurveValue& curve, const std::function<double (float)>& toValue,
                                 bool inDecibels)
        {
            std::vector<engine::CurvePoint> read;
            read.reserve (curve.points.size());

            for (const auto& point : curve.points)
                read.push_back ({ point.beat.inBeats(), point.position, point.curve });

            std::stable_sort (read.begin(), read.end(), [] (const auto& a, const auto& b) { return a.beat < b.beat; });
            const engine::Curve line (read);

            const auto add = [&points] (double beat, double value, bool held)
            {
                auto* element = points.createNewChildElement ("RealPoint");
                element->setAttribute ("time", number (beat));
                element->setAttribute ("value", number (value));
                element->setAttribute ("interpolation", held ? "hold" : "linear");
            };

            const auto isClose = [inDecibels] (double written, double played)
            {
                if (! inDecibels)
                    return std::abs (written - played) <= automationToleranceNormalized;

                const auto toDecibels = [] (double gain)
                { return gain > 0.0 ? std::max (automationFloorDb, 20.0 * std::log10 (gain)) : automationFloorDb; };
                return std::abs (toDecibels (written) - toDecibels (played)) <= automationToleranceDb;
            };

            // Points along a line from one beat to another, each value as Phazite plays it there: none where a straight line
            // keeps close, else the middle, and as many on each half.
            const std::function<void (double, double, double, double, int)> addAlong =
                [&] (double start, double startValue, double end, double endValue, int halvings)
            {
                auto close = true;

                for (int k = 1; k < probesPerPiece && close; ++k)
                {
                    const auto along = (double) k / probesPerPiece;
                    close = isClose (startValue + (endValue - startValue) * along,
                                     toValue (line.getPositionAt (start + (end - start) * along)));
                }

                if (close || halvings >= maximumHalvings)
                    return;

                const auto middle = (start + end) / 2.0;
                const auto middleValue = toValue (line.getPositionAt (middle));
                addAlong (start, startValue, middle, middleValue, halvings + 1);
                add (middle, middleValue, false);
                addAlong (middle, middleValue, end, endValue, halvings + 1);
            };

            for (size_t i = 0; i < read.size(); ++i)
            {
                const auto& from = read[i];
                const auto hasLine = i + 1 < read.size() && read[i + 1].beat > from.beat;
                const auto held = hasLine && from.curve >= 1.0f;
                add (from.beat, toValue (from.position), held);

                if (! hasLine || held)
                    continue;

                const auto& to = read[i + 1];

                if (from.curve <= -1.0f)
                    add (from.beat, toValue (to.position), false); // it jumps as it starts, and holds there
                else
                    addAlong (from.beat, toValue (from.position), to.beat, toValue (to.position), 0);
            }
        }
    };
} // namespace

DawProjectContent describeDawProject (const Song& song, const juce::String& title, const juce::String& comment)
{
    JUCE_ASSERT_MESSAGE_THREAD
    DawProjectContent content;
    Writer writer { song, content };
    writer.write (title, comment);
    return content;
}

namespace
{
    // What the tests have called before each part of a file the archive copies (setArchivePartSeam()).
    std::function<void (const std::function<bool()>&)> archivePartSeam;

    // The CRC-32 a zip keeps of each entry (ISO 3309's, as zlib's crc32()), taken on from what it was for the bytes before.
    std::uint32_t continueCrc (std::uint32_t crc, const void* data, size_t numBytes)
    {
        static const auto table = []
        {
            std::array<std::uint32_t, 256> values {};

            for (std::uint32_t n = 0; n < 256; ++n)
            {
                auto c = n;

                for (int k = 0; k < 8; ++k)
                    c = (c & 1) != 0 ? 0xedb88320u ^ (c >> 1) : c >> 1;

                values[n] = c;
            }

            return values;
        }();

        crc = ~crc;

        for (const auto byte : std::span (static_cast<const std::uint8_t*> (data), numBytes))
            crc = table[(crc ^ byte) & 0xffu] ^ (crc >> 8);

        return ~crc;
    }

    //==============================================================================
    // A zip written as it goes, in place of JUCE's ZipFile::Builder, which holds each entry whole in memory before it writes
    // it, can't be stopped, and goes on past a write that failed: an entry packed with deflate from memory (a text, a
    // preset), or a file from the disk stored as it is, a part at a time, its CRC and sizes written into its header once it
    // is all there; then the central directory. Sizes and places in 32 bits, without ZIP64, as JUCE's (checkArchiveFits()).
    // Each part of a file waits for shouldStop to say go on. JUCE workaround W41 (docs/juce-workarounds.md).
    class ArchiveWriter
    {
    public:
        ArchiveWriter (juce::FileOutputStream& stream, const std::function<bool()>& stop)
            : out (stream),
              shouldStop (stop)
        {
        }

        bool addPacked (const juce::String& path, const void* data, size_t numBytes, juce::Time time)
        {
            juce::MemoryOutputStream packed;

            {
                juce::GZIPCompressorOutputStream compressor (packed, 9, juce::GZIPCompressorOutputStream::windowBitsRaw);

                // Nothing written for nothing (the compressor takes no empty buffer): it packs it all the same.
                if (numBytes > 0 && ! compressor.write (data, numBytes))
                    return false;

                compressor.flush();
            }

            Entry entry { path,
                          time,
                          deflated,
                          continueCrc (0, data, numBytes),
                          (juce::int64) packed.getDataSize(),
                          (juce::int64) numBytes,
                          out.getPosition() };
            return writeHeader (entry) && out.write (packed.getData(), packed.getDataSize()) && keep (entry);
        }

        bool addStored (const juce::String& path, const juce::File& source)
        {
            juce::FileInputStream in (source);

            if (in.failedToOpen())
                return false;

            Entry entry { path, source.getLastModificationTime(), stored, 0, 0, 0, out.getPosition() };

            if (! writeHeader (entry))
                return false;

            juce::HeapBlock<char> part (partBytes);

            for (;;)
            {
                if (archivePartSeam != nullptr)
                    archivePartSeam (shouldStop);

                if (isStopped())
                    return false;

                const auto numRead = in.read (part, partBytes);

                if (numRead < 0)
                    return false;

                if (numRead == 0)
                    break;

                entry.crc = continueCrc (entry.crc, part, (size_t) numRead);
                entry.size += numRead;

                if (entry.size > maxArchiveBytes || ! out.write (part, (size_t) numRead))
                    return false;
            }

            // Its CRC and sizes into its header, now that they are known.
            entry.packedSize = entry.size;
            const auto end = out.getPosition();
            return out.setPosition (entry.offset + 14) && writeSizes (entry) && out.setPosition (end) && keep (entry);
        }

        // The central directory, and its end.
        bool finish()
        {
            const auto start = out.getPosition();
            auto ok = true;

            for (const auto& entry : entries)
            {
                ok = out.writeInt (0x02014b50) && out.writeShort (versionNeeded) && writeFlagsTo (entry) && out.writeShort (0)
                     && out.writeShort (0) && out.writeShort (0) && out.writeInt (0) && out.writeInt ((int) (std::uint32_t) entry.offset)
                     && writePath (entry) && ok;
            }

            const auto end = out.getPosition();
            ok = out.writeInt (0x06054b50) && out.writeShort (0) && out.writeShort (0) && out.writeShort ((short) entries.size())
                 && out.writeShort ((short) entries.size()) && out.writeInt ((int) (std::uint32_t) (end - start))
                 && out.writeInt ((int) (std::uint32_t) start) && out.writeShort (0) && ok;

            out.flush();
            return ok && out.getStatus().wasOk();
        }

        bool isStopped() const { return shouldStop != nullptr && shouldStop(); }

    private:
        static constexpr short stored = 0, deflated = 8;
        static constexpr short versionNeeded = 20; // 2.0: deflate
        static constexpr int partBytes = 1 << 20;

        struct Entry
        {
            juce::String path;
            juce::Time time;
            short method = stored;
            std::uint32_t crc = 0;
            juce::int64 packedSize = 0, size = 0, offset = 0;
        };

        juce::FileOutputStream& out;
        const std::function<bool()>& shouldStop;
        std::vector<Entry> entries;

        bool keep (const Entry& entry)
        {
            entries.push_back (entry);
            return true;
        }

        // From the version needed on, as a local header and the central directory have them: the version, the flags (the
        // path in UTF-8), the method, the time and date (as MS-DOS keeps them), the CRC, the sizes and the path's length,
        // and no extra field.
        bool writeFlagsTo (const Entry& entry)
        {
            const auto& t = entry.time;
            return out.writeShort (versionNeeded) && out.writeShort ((short) (1 << 11)) && out.writeShort (entry.method)
                   && out.writeShort ((short) ((t.getSeconds() / 2) + (t.getMinutes() << 5) + (t.getHours() << 11)))
                   && out.writeShort ((short) (t.getDayOfMonth() + ((t.getMonth() + 1) << 5) + ((t.getYear() - 1980) << 9)))
                   && writeSizes (entry) && out.writeShort ((short) entry.path.getNumBytesAsUTF8()) && out.writeShort (0);
        }

        bool writeSizes (const Entry& entry)
        {
            return out.writeInt ((int) entry.crc) && out.writeInt ((int) (std::uint32_t) entry.packedSize)
                   && out.writeInt ((int) (std::uint32_t) entry.size);
        }

        bool writePath (const Entry& entry) { return out.write (entry.path.toRawUTF8(), entry.path.getNumBytesAsUTF8()); }

        bool writeHeader (const Entry& entry) { return out.writeInt (0x04034b50) && writeFlagsTo (entry) && writePath (entry); }
    };
} // namespace

void setArchivePartSeam (std::function<void (const std::function<bool()>&)> seam)
{
    archivePartSeam = std::move (seam);
}

juce::Result writeDawProject (DawProjectContent& content, const juce::File& bouncesFolder, const juce::File& file,
                              const std::function<bool()>& shouldStop)
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

    // project.xml and metadata.xml, the files (stored: audio barely compresses) and the presets; refused at once if the archive
    // wouldn't fit what the zip writer holds. JUCE workaround W41 (docs/juce-workarounds.md).
    const auto now = juce::Time::getCurrentTime();
    std::vector<ArchiveEntry> entries;
    std::vector<juce::String> texts;

    for (const auto* xml : { content.project.get(), content.metadata.get() })
    {
        texts.push_back (xml->toString());
        entries.push_back ({ texts.size() == 1 ? "project.xml" : "metadata.xml", (juce::int64) texts.back().getNumBytesAsUTF8(), true });
    }

    for (const auto& [path, source] : content.files)
        entries.push_back ({ path, source.getSize(), false });

    for (const auto& [path, blob] : content.blobs)
        entries.push_back ({ path, (juce::int64) blob.getSize(), true });

    if (const auto fits = checkArchiveFits (entries); fits.failed())
        return fits;

    // Into a temporary file next to it, which takes its place once complete, and goes if it never does.
    const auto failed = juce::Result::fail ("Phazite couldn't write " + file.getFileName() + ".");
    file.getParentDirectory().createDirectory();
    juce::TemporaryFile temporary (file);

    {
        juce::FileOutputStream out (temporary.getFile());

        if (! out.openedOk())
            return failed;

        ArchiveWriter archive (out, shouldStop);
        auto written = true;

        for (size_t i = 0; i < texts.size() && written; ++i)
            written = archive.addPacked (entries[i].path, texts[i].toRawUTF8(), texts[i].getNumBytesAsUTF8(), now);

        for (size_t i = 0; i < content.files.size() && written; ++i)
            written = archive.addStored (content.files[i].first, content.files[i].second);

        for (size_t i = 0; i < content.blobs.size() && written; ++i)
            written = archive.addPacked (content.blobs[i].first, content.blobs[i].second.getData(), content.blobs[i].second.getSize(), now);

        if (! written || ! archive.finish())
            return archive.isStopped() ? juce::Result::fail ("Cancelled.") : failed;
    }

    // A file that grew while it was packed could still take it past: never one that says it worked.
    if (temporary.getFile().getSize() > maxArchiveBytes)
        return checkArchiveFits ({ { "the archive", temporary.getFile().getSize(), false } });

    if (! temporary.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Phazite couldn't replace " + file.getFileName() + ". Is it open in another app?");

    return juce::Result::ok();
}

juce::Result checkDawProjectFits (const DawProjectContent& content, double bounceSeconds)
{
    std::vector<ArchiveEntry> entries;

    for (const auto& [xml, path] :
         { std::pair { content.project.get(), "project.xml" }, std::pair { content.metadata.get(), "metadata.xml" } })
        entries.push_back ({ path, xml != nullptr ? (juce::int64) xml->toString().getNumBytesAsUTF8() : 0, true });

    for (const auto& [path, source] : content.files)
        entries.push_back ({ path, source.getSize(), false });

    for (const auto& [path, blob] : content.blobs)
        entries.push_back ({ path, (juce::int64) blob.getSize(), true });

    // A bounce's WAV file: its header and its samples, two channels of four bytes, at most.
    const auto bounceBytes = (juce::int64) 4096 + (juce::int64) std::ceil (std::max (0.0, bounceSeconds) * bounceRate) * 2 * 4;

    for (const auto& [track, clip] : content.bounces)
        entries.push_back ({ "bounces/" + juce::String ((juce::int64) track.getRaw()) + ".wav", bounceBytes, false });

    return checkArchiveFits (entries);
}

juce::Result checkArchiveFits (const std::vector<ArchiveEntry>& entries)
{
    // What the writer writes for each entry: its local header (30 bytes and its path), what it holds, packed at most as large
    // as deflate's bound (zlib's deflateBound()), and its place in the central directory (46 bytes and its path again); and
    // the directory's end, 22 bytes.
    juce::int64 total = 22;

    for (const auto& entry : entries)
    {
        const auto bytes = std::max ((juce::int64) 0, entry.bytes);
        const auto held = entry.packed ? bytes + (bytes >> 12) + (bytes >> 14) + (bytes >> 25) + 13 : bytes;
        total += 30 + 46 + 2 * (juce::int64) entry.path.getNumBytesAsUTF8() + held;
    }

    const auto gigabytes = [] (juce::int64 bytes) { return juce::String ((double) bytes / (1024.0 * 1024.0 * 1024.0), 1) + " GB"; };
    const auto instead = juce::String (" Export the song's audio beside it instead (Export Song, its stems), or export a version "
                                       "with fewer tracks or shorter audio.");

    if ((int) entries.size() > maxArchiveEntries)
        return juce::Result::fail ("The DAWproject would hold " + juce::String ((int) entries.size()) + " files, more than the "
                                   + juce::String (maxArchiveEntries) + " Phazite can write into one." + instead);

    if (total > maxArchiveBytes)
        return juce::Result::fail ("The DAWproject would be " + gigabytes (total)
                                   + ", more than the 4 GB Phazite can write into one "
                                     "for now."
                                   + instead);

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
            cancelled = true; // an archive being written stops soon
            render = nullptr; // cancels what is left, and waits for its thread
            ownPlugins = nullptr;
            bouncesFolder.deleteRecursively();
        }

        void start (PluginHost* pluginHost, const juce::String& comment)
        {
            JUCE_ASSERT_MESSAGE_THREAD
            auto* session = findSession (song);
            PluginInstances* instances = session != nullptr ? &session->getPlugins() : nullptr;

            // The plug-ins' states as they are now, which the song's VST3s go in with as their preset files. An old
            // version's song holds them as they were then.
            if (session != nullptr)
                song.storeDeviceStates();

            // A song no session plays (an old version's): its plug-ins in instances of its own, in the background, opened there
            // too (the review of known bug 166: the render's start opened each while the message thread waited), the export
            // begun once they are in, each played whole from its first block (not faded in as live playback does one that comes
            // in late).
            if (session == nullptr && pluginHost != nullptr)
            {
                ownPlugins = std::make_unique<PluginInstances> (song, *pluginHost, bounceRate);
                ownPlugins->sync();
                ownPlugins->whenLoaded (
                    [this, comment]
                    {
                        ownPlugins->forgetJoinsLate();
                        begin (ownPlugins.get(), comment);
                    });
                return;
            }

            begin (instances, comment);
        }

        void begin (PluginInstances* instances, const juce::String& comment)
        {
            auto* session = findSession (song);
            content = describeDawProject (song, title, comment);
            std::vector<OfflineRender::Job> jobs;
            const auto end = toSeconds (song, getSongEnd (song));

            // Refused before anything is rendered, where it wouldn't fit what the zip writer holds (JUCE workaround W41).
            if (const auto fits = checkDawProjectFits (content, end + tailSeconds); fits.failed())
            {
                juce::MessageManager::callAsync (
                    [weakAlive = std::weak_ptr<DawProjectExport*> (alive), fits]
                    {
                        if (const auto locked = weakAlive.lock())
                            (*locked)->finish (fits);
                    });
                return;
            }

            for (const auto& [track, clip] : content.bounces)
                jobs.push_back ({ bouncesFolder.getChildFile (juce::String ((juce::int64) track.getRaw()) + ".wav"), end, tailSeconds,
                                  SongPlayer::RenderOf { (std::int64_t) track.getRaw(), true, std::nullopt } });

            if (! jobs.empty())
            {
                bouncesFolder.createDirectory();

                if (session != nullptr && isSongPlaying (song))
                    stopAndReturn (song);
            }

            // The bounces' clips' renders that aren't there yet made on the render's thread first (OfflineRender.h).
            const OfflineRender::Setup setup { .session = jobs.empty() ? nullptr : session,
                                               .makePlayer = [this, instances] { return std::make_unique<SongPlayer> (song, instances); },
                                               .clipsOf = jobs.empty() ? nullptr : &song,
                                               .numThreads = session != nullptr ? session->getHost().getNumThreads() : 1,
                                               .sampleRate = bounceRate,
                                               .blockSize = bounceBlockSize,
                                               .background = session == nullptr };

            render = std::make_unique<OfflineRender> (
                setup, jobs, OfflineRender::Rendered(),
                [this, weakAlive = std::weak_ptr<DawProjectExport*> (alive)] (const juce::Result& rendered)
                {
                    // On the render's thread, once the bounces are: the zip, which
                    // stops between two parts of a file once this is cancelled.
                    const auto result =
                        rendered.wasOk() ? writeDawProject (content, bouncesFolder, file, [this] { return cancelled.load(); }) : rendered;
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
        std::optional<RenderWait> getWait() const override { return render != nullptr ? render->findWait() : std::nullopt; }

    private:
        Song& song;
        const juce::File file;
        const juce::String title;
        const std::function<void (const juce::Result&)> onFinished;
        std::shared_ptr<DawProjectExport*> alive;
        const juce::File bouncesFolder =
            juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("Phazite DAWproject", {}, false);
        DawProjectContent content;
        std::atomic<bool> cancelled { false };       // what the archive's write asks, on the render's thread
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

    // The attributes the reader below reads as numbers (readNumber(), readValue(), getDoubleAttribute(), getIntAttribute()),
    // and nothing else: a device's ID, a path or a name can look like such a number ("5E12345678901...").
    bool isReadAsNumber (const juce::String& attribute)
    {
        static const juce::StringArray names { "time",        "duration",   "value",       "vel",       "playStart",   "playStop",
                                               "contentTime", "fadeInTime", "fadeOutTime", "numerator", "denominator", "key" };
        return names.contains (attribute);
    }

    // Each of those whose exponent JUCE's reader would overflow (model/base/Json.h: hasExponentTooLongToRead()) made the same
    // number in a text it reads.
    void makeExponentsReadable (juce::XmlElement& element)
    {
        for (int i = element.getNumAttributes(); --i >= 0;)
            if (const auto& value = element.getAttributeValue (i);
                isReadAsNumber (element.getAttributeName (i)) && hasExponentTooLongToRead (value))
                element.setAttribute (element.getAttributeName (i), makeExponentReadable (value));

        for (auto* child : element.getChildIterator())
            makeExponentsReadable (*child);
    }
} // namespace

std::unique_ptr<TrackRender> startDawProjectExport (Song& song, PluginHost* pluginHost, const juce::File& file, const juce::String& title,
                                                    const juce::String& comment, std::function<void (const juce::Result&)> onFinished)
{
    auto exporting = std::make_unique<DawProjectExport> (song, file, title, std::move (onFinished));
    exporting->start (pluginHost, comment);
    return std::make_unique<TrackRender> (std::move (exporting));
}

//==============================================================================
juce::int64 getUnpackLimit (const juce::File& archive)
{
    return std::max ((juce::int64) 64 << 20, archive.getSize() * 1100);
}

//==============================================================================
namespace
{
    std::atomic<juce::int64> bytesUnpackedOnMessageThread { 0 };

    bool holdsTheSameAs (juce::ZipFile& zip, int index, const juce::File& file)
    {
        const std::unique_ptr<juce::InputStream> entry (zip.createStreamForEntry (index));
        juce::FileInputStream existing (file);

        if (entry == nullptr || ! existing.openedOk())
            return false;

        juce::HeapBlock<char> one (1 << 16), other (1 << 16);

        for (;;)
        {
            const auto read = existing.read (one, 1 << 16);

            if (read <= 0)
                return entry->read (other, 1) <= 0;

            if (entry->read (other, read) != read || std::memcmp (one, other, (size_t) read) != 0)
                return false;
        }
    }

    // A file of the zip copied into a folder: not at all where it has a file of its name that holds the same (a DAWproject
    // Phazite wrote, coming back); no more than the entry says it holds, nor, with what came out before it (unpacked), than
    // an archive of this size can hold (getUnpackLimit()): a hostile archive never fills the disk (entries can share their
    // compressed bytes), one that doesn't fit refused before anything of it is written. In blocks, asking between them
    // whether to stop (a cancelled import), which takes back what was begun. The file, none for what can't be unpacked; and
    // whether it was made here.
    std::pair<juce::File, bool> unpackEntry (juce::ZipFile& zip, const juce::String& path, const juce::File& folder, juce::int64 limit,
                                             juce::int64& unpacked, const std::function<bool()>& shouldStop)
    {
        const auto index = zip.getIndexOfFileName (path);

        if (index < 0 || folder == juce::File())
            return {};

        // The entry's name, from its path inside the archive: relative, which a juce::File made from it asserts against.
        const auto entry = juce::File::createFileWithoutCheckingPath (
            path.fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false));
        const auto name = makeUsableFileName (entry.getFileNameWithoutExtension()); // never one Windows keeps for a device
        const auto declared = zip.getEntry (index)->uncompressedSize;

        if (const auto same = folder.getChildFile (name + entry.getFileExtension());
            same.existsAsFile() && same.getSize() == declared && holdsTheSameAs (zip, index, same))
            return { same, false };

        if (declared < 0 || declared > limit - unpacked)
            return {};

        const auto target = folder.getNonexistentChildFile (name, entry.getFileExtension(), false);
        const std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (index));
        folder.createDirectory();

        if (stream == nullptr)
            return {};

        auto whole = false;

        {
            juce::FileOutputStream out (target);

            if (! out.openedOk())
            {
                target.deleteFile();
                return {};
            }

            juce::HeapBlock<char> block (1 << 16);
            juce::int64 written = 0;

            while (written < declared && ! shouldStop())
            {
                const auto read = stream->read (block, (int) std::min ((juce::int64) (1 << 16), declared - written));

                if (read <= 0 || ! out.write (block, (size_t) read))
                    break;

                written += read;
            }

            unpacked += written;

            if (juce::MessageManager::existsAndIsCurrentThread())
                bytesUnpackedOnMessageThread += written;

            char beyond = 0;
            whole = written == declared && stream->read (&beyond, 1) <= 0;
        }

        if (! whole)
        {
            target.deleteFile();
            return {};
        }

        return { target, true };
    }

    // The paths inside the archive of the audio files its clips name (Audio elements' File), each once, in order.
    juce::StringArray findAudioPaths (const juce::XmlElement& project)
    {
        juce::StringArray paths;
        const std::function<void (const juce::XmlElement&)> visit = [&] (const juce::XmlElement& element)
        {
            for (const auto* child : element.getChildIterator())
            {
                if (child->hasTagName ("Audio"))
                    if (const auto* file = child->getChildByName ("File"))
                        paths.addIfNotAlreadyThere (file->getStringAttribute ("path"));

                visit (*child);
            }
        };

        visit (project);
        paths.removeEmptyStrings();
        return paths;
    }
} // namespace

DawProjectFile readDawProject (const juce::File& file)
{
    DawProjectFile read;
    read.file = file;
    juce::ZipFile zip (file);
    const auto limit = std::min (maxDawProjectText, getUnpackLimit (file));

    // Read whole, up to what such a text can be: one larger is no DAWproject Phazite reads.
    const auto parse = [&zip, limit] (const char* name) -> std::unique_ptr<juce::XmlElement>
    {
        const auto index = zip.getIndexOfFileName (name);
        const std::unique_ptr<juce::InputStream> stream (index >= 0 ? zip.createStreamForEntry (index) : nullptr);
        juce::MemoryBlock text;

        if (stream == nullptr || (juce::int64) stream->readIntoMemoryBlock (text, (std::ptrdiff_t) limit + 1) > limit)
            return nullptr;

        // Text that is no UTF-8 is no XML a DAWproject holds.
        if (! juce::CharPointer_UTF8::isValidString (static_cast<const char*> (text.getData()), (int) text.getSize()))
            return nullptr;

        // As readEntireStreamAsString() reads it: without a byte order mark at its start; and with each number whose
        // exponent JUCE's reader would overflow made one it reads, before any is read (known bug 170).
        auto xml = juce::parseXML (juce::String::createStringFromData (text.getData(), (int) text.getSize()));

        if (xml != nullptr)
            makeExponentsReadable (*xml);

        return xml;
    };

    read.project = parse ("project.xml");
    read.metadata = parse ("metadata.xml");

    if (read.project == nullptr || read.project->getTagName() != "Project")
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

    double readNumber (const juce::XmlElement&, const char* name, double otherwise, double low, double high);

    // A DAWproject's tracks, by their elements' ids and in order: those that are the song's (not the master, nor a bounce
    // Phazite added, nor a folder, whose tracks are among them, and which are its groups), and their lanes.
    struct ReadTracks
    {
        std::vector<const juce::XmlElement*> tracks, groups;
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
                {
                    groups.push_back (track);
                    collect (*track);
                }
                else if (! isBounce (*track))
                    tracks.push_back (track);

                byID[track->getStringAttribute ("id")] = track;
            }
        }

        static bool isBounce (const juce::XmlElement& track) { return track.getStringAttribute ("name").endsWith (" (bounce)"); }

        // A lane's clips, in order: those with notes, or those that play audio (findAudio()).
        std::vector<const juce::XmlElement*> getClips (const juce::XmlElement& track, bool withNotes) const
        {
            std::vector<const juce::XmlElement*> clips;

            if (const auto* lane = findLane (track))
                for (const auto* group : lane->getChildWithTagNameIterator ("Clips"))
                    for (const auto* clip : group->getChildWithTagNameIterator ("Clip"))
                        if ((clip->getChildByName ("Notes") != nullptr) == withNotes && (withNotes || findAudio (*clip).audio != nullptr))
                            clips.push_back (clip);

            return clips;
        }

        const juce::XmlElement* findLane (const juce::XmlElement& track) const
        {
            const auto lane = lanes.find (track.getStringAttribute ("id"));
            return lane != lanes.end() ? lane->second : nullptr;
        }

        // What an audio clip plays: its Audio, in the clip itself, in its Warps, or in a clip of its own Clips (as Bitwig
        // Studio writes one, a timeline of audio events of its own), the first there; the clip that holds it (the event, whose
        // playStart, content's unit and fades apply), its Warps if any, and where the event starts in beats from the clip.
        struct AudioEvent
        {
            const juce::XmlElement *audio = nullptr, *event = nullptr, *warps = nullptr;
            double offset = 0.0;
        };

        static AudioEvent findAudio (const juce::XmlElement& clip, int depth = 0)
        {
            if (const auto* audio = clip.getChildByName ("Audio"))
                return { audio, &clip, nullptr, 0.0 };

            if (const auto* warps = clip.getChildByName ("Warps"); warps != nullptr && warps->getChildByName ("Audio") != nullptr)
                return { warps->getChildByName ("Audio"), &clip, warps, 0.0 };

            if (const auto* inner = clip.getChildByName ("Clips"); inner != nullptr && depth < 4)
            {
                for (const auto* event : inner->getChildWithTagNameIterator ("Clip"))
                {
                    if (auto found = findAudio (*event, depth + 1); found.audio != nullptr)
                    {
                        found.offset += readNumber (*event, "time", 0.0, -maxSongBeats, maxSongBeats)
                                        - readNumber (clip, "playStart", 0.0, -maxSongBeats, maxSongBeats);
                        return found;
                    }
                }
            }

            return {};
        }

        static int countAudioEvents (const juce::XmlElement& clip)
        {
            const auto* inner = clip.getChildByName ("Clips");
            return inner != nullptr ? inner->getNumChildElements() : 1;
        }
    };

    double readValue (const juce::XmlElement* channel, const char* parameter, double otherwise)
    {
        const auto* element = channel != nullptr ? channel->getChildByName (parameter) : nullptr;
        return element != nullptr ? element->getDoubleAttribute ("value", otherwise) : otherwise;
    }

    // A number a file says, as Phazite takes it: within a range, and what it is otherwise for what is no finite number.
    double readNumber (const juce::XmlElement& element, const char* name, double otherwise, double low, double high)
    {
        const auto value = element.getDoubleAttribute (name, otherwise);
        return std::isfinite (value) ? juce::jlimit (low, high, value) : otherwise;
    }

    // Places and lengths in beats, and times in a file, in seconds.
    constexpr double maxSeconds = 1.0e7;

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
        juce::StringArray notTaken {};
        juce::int64 unpacked = 0;                // what copyOut() has written, against getUnpackLimit()
        const UnpackedAudio* prepared = nullptr; // what was unpacked already, off the message thread, if it was

        void take()
        {
            const auto written = describeDawProject (song, {}, {});
            const ReadTracks theirs (*in.project), ours (*written.project);
            const auto isNew = getTrackIDs (song).size() <= 1 && getClipCount() == 0;
            std::set<const juce::XmlElement*> matched;
            std::vector<TrackID> gone;

            // Into a new song, the tempo, the time signature and the markers first: what its clips are placed by.
            if (isNew)
                takeTransport();

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

            // The master's fader.
            if (theirs.master != nullptr && ours.master != nullptr)
                takeFader ({}, theirs.master->getChildByName ("Channel"), ours.master->getChildByName ("Channel"));

            // What a new song has no place for: the groups (their tracks came in on their own), and the master's devices.
            if (isNew)
            {
                for (const auto* group : theirs.groups)
                    notTaken.add (getName (*group) + ": a group, whose tracks came in on their own, without its channel");

                if (theirs.master != nullptr)
                    tellChannel (*theirs.master, theirs);
            }
        }

        static juce::String getName (const juce::XmlElement& track)
        {
            const auto name = track.getStringAttribute ("name").trim();
            return name.isNotEmpty() ? name : juce::String ("A track");
        }

        // What a track's channel holds that isn't taken: its devices, each by its name, its sends, and its automation.
        void tellChannel (const juce::XmlElement& track, const ReadTracks& theirs)
        {
            const auto* channel = track.getChildByName ("Channel");
            const auto name = getName (track);

            if (const auto* devices = channel != nullptr ? channel->getChildByName ("Devices") : nullptr;
                devices != nullptr && devices->getNumChildElements() > 0)
            {
                juce::StringArray names;

                for (const auto* device : devices->getChildIterator())
                    names.add (device->getStringAttribute ("deviceName", device->getStringAttribute ("name", device->getTagName())));

                notTaken.add (name + ": its devices (" + names.joinIntoString (", ") + ")");
            }

            if (const auto* sends = channel != nullptr ? channel->getChildByName ("Sends") : nullptr;
                sends != nullptr && sends->getNumChildElements() > 0)
                notTaken.add (name + ": its sends");

            if (const auto* lane = theirs.findLane (track); lane != nullptr && hasAutomation (*lane))
                notTaken.add (name + ": its automation");
        }

        static bool hasAutomation (const juce::XmlElement& lane)
        {
            for (const auto* child : lane.getChildIterator())
                if (child->getTagName() == "Points" || (child->getTagName() == "Lanes" && hasAutomation (*child)))
                    return true;

            return false;
        }

        int getClipCount() const { return (int) getAllClips (song).size(); }

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
            std::vector<ClipID> ourClips;

            for (const auto& clip : getClipValues (song, id))
                if (! clip.isAudio())
                    ourClips.push_back (clip.id);

            if (ourClips.size() != clips.size())
                notTaken.addIfNotAlreadyThere (getTrackName (song, id) + ": its clips added or taken away");

            for (size_t i = 0; i < std::min (ourClips.size(), clips.size()); ++i)
            {
                const auto clipID = ourClips[i];
                const auto contentStart = getContentStart (song, clipID);
                std::vector<NoteData> unused; // the song's notes (Notes.h), each where it is among the clip's notes

                for (const auto& note : getClipNotes (song, clipID))
                    if (! note.muted && ! note.slide)
                        unused.push_back (note);

                std::vector<const juce::XmlElement*> added;

                for (const auto* note : clips[i]->getChildByName ("Notes")->getChildWithTagNameIterator ("Note"))
                {
                    const auto same = std::find_if (
                        unused.begin(), unused.end(),
                        [note, contentStart] (const NoteData& ours)
                        {
                            return ours.pitch == note->getIntAttribute ("key") && ours.channel == readNoteChannel (*note)
                                   && isClose ((ours.start - contentStart).inBeats(), note->getDoubleAttribute ("time"))
                                   && isClose (ours.length.inBeats(), note->getDoubleAttribute ("duration"))
                                   && std::abs (ours.velocity - roundToNearestInt (note->getDoubleAttribute ("vel", 0.8) * 127.0)) <= 1;
                        });

                    if (same != unused.end())
                        unused.erase (same);
                    else
                        added.push_back (note);
                }

                // Those it has that the DAWproject hasn't go, from this clip alone, as each of its clips is taken in.
                std::vector<NoteID> gone;

                for (const auto& note : unused)
                    gone.push_back (note.id);

                removeClipNotesAlone (song, clipID, gone);
                std::vector<NoteData> notesAdded;

                for (const auto* note : added)
                    notesAdded.push_back (readNote (clipID, contentStart, *note));

                addNotes (song, notesAdded);
                keepWithinSong (clipID);
            }
        }

        // A clip grows to hold a note put past its end: no longer than a song's clips can be, for a note a million beats
        // along.
        void keepWithinSong (ClipID id)
        {
            if (auto clip = getClipValue (song, id); clip.has_value() && clip->length.inBeats() > maxSongBeats)
            {
                clip->length = BeatDuration::fromBeats (maxSongBeats);
                setClip (song, *clip);
            }
        }

        // A note's MIDI channel, which DAWproject counts from 0: as the song counts them, from 1 to 16.
        static int readNoteChannel (const juce::XmlElement& note)
        {
            return juce::roundToInt (readNumber (note, "channel", 0.0, 0.0, 15.0)) + 1;
        }

        // A DAWproject's note as a note to add to a clip (Notes.h: addNotes()).
        static NoteData readNote (ClipID clip, BeatPosition contentStart, const juce::XmlElement& note)
        {
            NoteData data;
            data.clip = clip;
            data.pitch = juce::jlimit (0, 127, note.getIntAttribute ("key"));
            data.start = contentStart + BeatDuration::fromBeats (readNumber (note, "time", 0.0, -maxSongBeats, maxSongBeats));
            data.length = BeatDuration::fromBeats (readNumber (note, "duration", 1.0, 1.0 / 64.0, maxSongBeats));
            data.velocity = juce::jlimit (1, 127, juce::roundToInt (readNumber (note, "vel", 0.8, 0.0, 1.0) * 127.0));
            data.channel = readNoteChannel (note);
            return data;
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
            tellChannel (track, theirs);

            for (const auto* clip : theirs.getClips (track, true))
            {
                // A clip with its track's name gets none of its own: it is called after its track, as Phazite's are
                // (getClipName()).
                const auto start = readNumber (*clip, "time", 0.0, 0.0, maxSongBeats);
                const auto name = clip->getStringAttribute ("name");
                auto made = makeMidiClip (song, name == getTrackName (song, id) ? juce::String() : name,
                                          { BeatPosition::fromBeats (start),
                                            BeatPosition::fromBeats (start + readNumber (*clip, "duration", 4.0, 0.25, maxSongBeats)) });
                made.track = id;
                made.offset = BeatDuration::fromBeats (readNumber (*clip, "playStart", 0.0, 0.0, maxSongBeats));
                const auto clipID = addClip (song, made);

                if (! clipID.isValid())
                    continue;

                const auto contentStart = getContentStart (song, clipID);
                std::vector<NoteData> notes;

                for (const auto* note : clip->getChildByName ("Notes")->getChildWithTagNameIterator ("Note"))
                    notes.push_back (readNote (clipID, contentStart, *note));

                addNotes (song, notes);
                keepWithinSong (clipID);
            }

            juce::StringArray audioNotTaken;

            for (const auto* clip : theirs.getClips (track, false))
                if (const auto why = addAudioClip (id, *clip); why.isNotEmpty())
                    audioNotTaken.addIfNotAlreadyThere (why);

            for (const auto& why : audioNotTaken)
                notTaken.add (getName (track) + ": " + why);
        }

        // An audio clip: its file copied out of the zip into the song's Audio folder, once for all the clips that play it, the
        // part of it the clip plays, its fades; and, where its warps stretch it (as Bitwig Studio's do to fit a loop to the
        // tempo), following the tempo at the tempo the warps give it. What of it isn't taken, or nothing.
        juce::String addAudioClip (TrackID track, const juce::XmlElement& clip)
        {
            const auto found = ReadTracks::findAudio (clip);
            const auto* file = found.audio != nullptr ? found.audio->getChildByName ("File") : nullptr;
            const auto* warps = found.warps;
            const auto& event = *found.event;

            if (file == nullptr)
                return "audio clips without a file";

            // Where it is in the file is in seconds: as the clip's content says, or as its warps map it.
            struct Warp
            {
                double time = 0.0, contentTime = 0.0;
            };

            std::vector<Warp> points;

            if (warps != nullptr && warps->getStringAttribute ("contentTimeUnit") == "seconds"
                && warps->getStringAttribute ("timeUnit", "beats") == "beats")
                for (const auto* warp : warps->getChildWithTagNameIterator ("Warp"))
                    points.push_back ({ readNumber (*warp, "time", 0.0, -maxSongBeats, maxSongBeats),
                                        readNumber (*warp, "contentTime", 0.0, 0.0, maxSeconds) });

            std::stable_sort (points.begin(), points.end(), [] (const Warp& a, const Warp& b) { return a.time < b.time; });
            const auto warped =
                points.size() >= 2 && points.back().time > points.front().time && points.back().contentTime > points.front().contentTime;

            if (! warped && event.getStringAttribute ("contentTimeUnit", "beats") != "seconds")
                return "audio clips timed in beats without warps";

            const auto copied = copyOut (file->getStringAttribute ("path"));

            if (copied == juce::File())
                return "audio clips whose files aren't in it";

            const auto start = readNumber (clip, "time", 0.0, 0.0, maxSongBeats) + found.offset + (warped ? points.front().time : 0.0);
            const auto from =
                std::min (maxSeconds - 0.001, warped ? points.front().contentTime : readNumber (event, "playStart", 0.0, 0.0, maxSeconds));
            const auto to = warped ? points.back().contentTime
                                   : readNumber (event, "playStop", readNumber (*found.audio, "duration", from + 1.0, 0.0, maxSeconds), 0.0,
                                                 maxSeconds);
            auto made = makeAudioClip (song, clip.getStringAttribute ("name", copied.getFileNameWithoutExtension()), copied,
                                       BeatPosition::fromBeats (std::max (0.0, start)),
                                       { from, std::min (maxSeconds, std::max (from + 0.001, to)) });
            made.track = track;

            // Fades in seconds, or in beats at the song's tempo.
            if (const auto unit = clip.getStringAttribute ("fadeTimeUnit", "seconds"); unit == "seconds" || unit == "beats")
            {
                const auto toSeconds = unit == "beats" ? 60.0 / getTempo (song) : 1.0;
                made.fadeIn.seconds = readNumber (clip, "fadeInTime", 0.0, 0.0, maxSeconds) * toSeconds;
                made.fadeOut.seconds = readNumber (clip, "fadeOutTime", 0.0, 0.0, maxSeconds) * toSeconds;
            }

            const auto id = addClip (song, made);

            if (id.isValid() && warped)
            {
                setClipFollowsTempo (song, id, true);
                setClipTempo (song, id,
                              (points.back().time - points.front().time) * 60.0 / (points.back().contentTime - points.front().contentTime));
            }

            if (points.size() > 2)
                return "the warps inside its audio clips (each plays straight from its first to its last)";

            if (ReadTracks::countAudioEvents (clip) > 1)
                return "the audio events after the first in its audio clips";

            return {};
        }

        // A file of the zip copied into the song's Audio folder: once, however many clips play it; and not at all where the
        // Audio folder has a file of its name that holds the same (a DAWproject Phazite wrote, coming back).
        std::map<juce::String, juce::File> unpackedFiles {};

        juce::File copyOut (const juce::String& path)
        {
            // Unpacked already, off the message thread (unpackDawProjectAudio()): what it unpacked, and nothing more.
            if (prepared != nullptr)
            {
                const auto found = prepared->files.find (path);
                return found != prepared->files.end() ? found->second : juce::File();
            }

            if (const auto done = unpackedFiles.find (path); done != unpackedFiles.end())
                return done->second;

            auto& file = unpackedFiles[path];
            file = unpack (path);
            return file;
        }

        juce::File unpack (const juce::String& path)
        {
            juce::ZipFile zip (in.file);
            const auto folder = song.getFile() != juce::File() ? song.getFile().getSiblingFile ("Audio") : juce::File();
            return unpackEntry (zip, path, folder, getUnpackLimit (in.file), unpacked, [] { return false; }).first;
        }

        void takeTransport()
        {
            if (const auto* transport = in.project->getChildByName ("Transport"))
            {
                if (const auto* tempo = transport->getChildByName ("Tempo"))
                    setTempo (song, readNumber (*tempo, "value", 120.0, minTempo, maxTempo));

                if (const auto* signature = transport->getChildByName ("TimeSignature"))
                    setTimeSignature (song, { signature->getIntAttribute ("numerator", 4), signature->getIntAttribute ("denominator", 4) });
            }

            const auto* arrangement = in.project->getChildByName ("Arrangement");

            if (const auto* automation = arrangement != nullptr ? arrangement->getChildByName ("TempoAutomation") : nullptr)
            {
                const auto* lanes = arrangement->getChildByName ("Lanes");
                const auto unit =
                    automation->getStringAttribute ("timeUnit", lanes != nullptr ? lanes->getStringAttribute ("timeUnit") : "");
                takeTempoAutomation (*automation, unit == "seconds");
            }

            const auto* markers = arrangement != nullptr ? arrangement->getChildByName ("Markers") : nullptr;
            std::vector<const juce::XmlElement*> all;

            if (markers != nullptr)
                for (const auto* marker : markers->getChildWithTagNameIterator ("Marker"))
                    all.push_back (marker);

            // Each a section, up to the next.
            for (size_t i = 0; i < all.size(); ++i)
            {
                const auto start = readNumber (*all[i], "time", 0.0, 0.0, maxSongBeats);
                const auto end = i + 1 < all.size() ? readNumber (*all[i + 1], "time", 0.0, 0.0, maxSongBeats) : start + 32.0;
                SectionInfo section;
                section.name = all[i]->getStringAttribute ("name");
                section.length = BeatDuration::fromBeats (std::max (1.0, end - start));
                insertSection (song, BeatPosition::fromBeats (start), section);
            }
        }

        // The tempo's changes: each point's tempo from its time, held until the next or ramping straight to it. Times in
        // seconds become beats along the tempos before them, a ramp's as straight in time, which its ends keep.
        void takeTempoAutomation (const juce::XmlElement& automation, bool inSeconds)
        {
            struct Point
            {
                double time = 0.0, bpm = 120.0;
                bool ramps = false;
            };

            std::vector<Point> points;

            for (const auto* point : automation.getChildWithTagNameIterator ("RealPoint"))
                points.push_back ({ readNumber (*point, "time", 0.0, -maxSongBeats, maxSongBeats),
                                    readNumber (*point, "value", 120.0, minTempo, maxTempo),
                                    point->getStringAttribute ("interpolation") == "linear" });

            std::stable_sort (points.begin(), points.end(), [] (const Point& a, const Point& b) { return a.time < b.time; });

            if (points.empty())
                return;

            std::vector<TempoChange> changes;
            auto beat = 0.0;

            for (size_t i = 0; i < points.size(); ++i)
            {
                if (! inSeconds)
                    beat = points[i].time;
                else if (i == 0)
                    beat = points[0].time * getTempo (song) / 60.0; // before it, the transport's tempo
                else
                    beat += (points[i].time - points[i - 1].time)
                            * (points[i - 1].ramps ? (points[i - 1].bpm + points[i].bpm) / 2.0 : points[i - 1].bpm) / 60.0;

                changes.push_back (
                    { BeatPosition::fromBeats (std::max (0.0, beat)), points[i].bpm, points[i].ramps ? tempoRamp : tempoJump });
            }

            // Before the first, if it starts later, the transport's tempo holds.
            takeTempoChanges (song, changes, BeatPosition());
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

juce::StringArray takeDawProject (Song& song, const DawProjectFile& in, const UnpackedAudio& unpacked)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (in.project == nullptr)
        return { in.error };

    Reader reader { song, in };
    reader.prepared = &unpacked;
    reader.take();
    return reader.notTaken;
}

UnpackedAudio unpackDawProjectAudio (const DawProjectFile& in, const juce::File& audioFolder, const std::function<bool()>& shouldStop,
                                     const std::function<void (float)>& progress)
{
    UnpackedAudio unpacked;

    if (in.project == nullptr)
        return unpacked;

    // The archive opened once for all of them.
    juce::ZipFile zip (in.file);
    const auto paths = findAudioPaths (*in.project);
    const auto limit = getUnpackLimit (in.file);
    juce::int64 total = 0;

    for (int i = 0; i < paths.size(); ++i)
    {
        if (shouldStop != nullptr && shouldStop())
        {
            unpacked.stopped = true;
            break;
        }

        if (progress != nullptr)
            progress ((float) i / (float) paths.size());

        const auto [file, made] =
            unpackEntry (zip, paths[i], audioFolder, limit, total, shouldStop != nullptr ? shouldStop : [] { return false; });

        if (file != juce::File())
            unpacked.files.emplace (paths[i], file);

        if (made)
            unpacked.made.push_back (file);
    }

    // Each at a path that was free, which may be one the song played before its file went (deleted meanwhile): what plays
    // the song asks the disk about its files again at its next plan (model/engine/AudioFiles.h; known bug 198).
    if (! unpacked.made.empty())
        noteFilesWritten();

    unpacked.stopped = unpacked.stopped || (shouldStop != nullptr && shouldStop());
    return unpacked;
}

void forgetUnusedAudio (const UnpackedAudio& unpacked, const Song& song)
{
    // What the song plays, by its files (its clips read by their IDs: AudioClips.h): a path within the project's folder, or a
    // full one.
    const auto folder = song.getFile().getParentDirectory();
    std::set<juce::File> played;

    for (const auto& path : findAudioFilePaths (song))
        played.insert (juce::File::isAbsolutePath (path) ? juce::File (path) : folder.getChildFile (path));

    for (const auto& file : unpacked.made)
        if (played.count (file) == 0)
            file.deleteFile();
}

juce::int64 countBytesUnpackedOnMessageThread()
{
    return bytesUnpackedOnMessageThread.load();
}

bool importDawProjectAsBranch (ProjectContext& context, std::shared_ptr<const DawProjectFile> in,
                               const std::function<void (std::function<void (bool saved)>)>& saveProjectThen, ImportDone done)
{
    auto* history = getVersionHistory (context);
    auto* open = findSong (context);
    const auto* version = history != nullptr && in != nullptr ? history->findVersion (in->fromVersion) : nullptr;
    const auto refuse = [&done] (const juce::String& why)
    {
        if (done != nullptr)
            done (false, { why });

        return false;
    };

    if (open == nullptr || version == nullptr || in == nullptr || in->project == nullptr)
        return refuse ("It wasn't exported from a version of this project.");

    // Now left on its branch, never a version open in its place (docs/design/versions.md: A version open).
    if (! backToNow (context))
        return refuse (whyNotBackToNow (context));

    if (const auto whyNot = whyNotSwitchBranch (context); whyNot.isNotEmpty())
        return refuse (whyNot);

    if (! canSwitchBranch (context))
        return refuse ("Not while the song records, renders, or a version is being saved.");

    // In steps, the window held from the first to the last (known bug 126): the version's song read on the history's thread;
    // made a song of its own here, with what came back taken in (its audio copied into the project); split and kept on the
    // history's thread; a branch from the version, with it, made here; and the switch to it, read there and made here, as any
    // switch is. What it adds takes IDs past the open song's, which no other branch has. A branch made is taken away again if
    // the switch doesn't finish, so that a cancelled import leaves nothing; done is told what came of it once it is over.
    struct Import
    {
        juce::StringArray said;
        juce::String branch;

        // Its audio, unpacked on the history's thread (known bug 126's review): what came of it, once it has, and whether to
        // stop, or to take it back as soon as it is done (an import cancelled or failed meanwhile).
        std::mutex lock;
        UnpackedAudio audio;
        bool unpacked = false, takeBack = false;
        std::atomic<bool> stop { false };
    };

    auto import = std::make_shared<Import>();
    const auto fromVersion = version->id;
    const auto nothing = juce::String ("Nothing was imported.");

    const auto takeInSong = [&context, in, import, fromVersion, nothing] (juce::ValueTree state) -> VersionHistory::Taken
    {
        auto* reading = getVersionHistory (context);
        auto* songNow = findSong (context);
        const auto song = state.isValid() && songNow != nullptr ? makeSong (state, songNow->getFile(), songNow->getCacheFolder()) : nullptr;

        if (reading == nullptr || song == nullptr)
            return VersionHistory::Taken::refused ("Phazite couldn't read the version it came from. " + nothing);

        song->getState().setProperty (schema::nextID, (juce::int64) std::max (findNextID (*song), findNextID (*songNow)), nullptr);
        import->said = takeDawProject (*song, *in, import->audio); // unpacked already: nothing more on this thread

        return VersionHistory::Taken::readAgain (
            [&context, import, fromVersion, nothing, copy = copyWholeSong (*song), store = reading->getStore()]() -> VersionHistory::TakeIn
            {
                const auto pieces = splitSong (copy);
                const auto kept = store->writeSong (pieces);
                store->getPieces().closeFiles();

                return [&context, import, fromVersion, nothing, kept, piece = pieces.song]() -> VersionHistory::Taken
                {
                    auto* making = getVersionHistory (context);

                    if (making == nullptr)
                        return false;

                    import->branch = kept ? making->makeBranchWithSong (fromVersion, piece) : juce::String();
                    auto plan = making->prepareSwitch (import->branch, true);

                    if (! plan.has_value())
                        return VersionHistory::Taken::refused ("Phazite couldn't make a branch for it. " + nothing);

                    return VersionHistory::Taken::readAgain (
                        [&context, plan = *plan, store = making->getStore()]() -> VersionHistory::TakeIn
                        {
                            auto read = std::make_shared<VersionHistory::SwitchRead> (VersionHistory::readSwitch (*store, plan));
                            return takeInSwitch (context, plan, read, 2);
                        });
                };
            });
    };

    // How far its unpacking is, for the progress window.
    const auto progress = std::make_shared<std::atomic<float>> (-1.0f);

    return startCommandRead (
        context, *history, "Reading \"" + getDisplayName (*version) + "\" to import " + in->file.getFileName(),
        [store = history->getStore(), piece = version->song, takeInSong, import, in, audioFolder = open->getFile().getSiblingFile ("Audio"),
         progress = progress]() -> VersionHistory::TakeIn
        {
            auto state = readCommitSong (*store, piece);
            store->getPieces().closeFiles();
            auto audio = unpackDawProjectAudio (
                *in, audioFolder, [import] { return import->stop.load(); }, [progress] (float fraction) { *progress = fraction; });

            {
                const std::scoped_lock held (import->lock);
                import->audio = std::move (audio);
                import->unpacked = true;

                if (import->takeBack)
                    for (const auto& file : import->audio.made)
                        file.deleteFile();
            }

            return [takeInSong, state] { return takeInSong (state); };
        },
        [&context, import, done, saveProjectThen] (bool did)
        {
            auto* historyNow = getVersionHistory (context);

            // The audio it unpacked that nothing plays taken away: all of it, where it didn't finish (as soon as it is done
            // unpacking, if it isn't yet).
            {
                import->stop = ! did;
                const std::scoped_lock held (import->lock);

                if (! import->unpacked)
                    import->takeBack = true;
                else if (auto* song = findSong (context); did && song != nullptr)
                    forgetUnusedAudio (import->audio, *song);
                else if (import->branch.isEmpty()) // else once the branch made, whose song names them, is gone (below)
                    for (const auto& file : import->audio.made)
                        file.deleteFile();
            }

            if (did)
                saveProjectThen (
                    [&context] (bool saved)
                    {
                        // As every switch: the branch's record names the song made for it until the project file holds it.
                        if (auto* now = getVersionHistory (context); now != nullptr && saved)
                            now->switchSaved();
                    });
            else if (historyNow != nullptr && import->branch.isNotEmpty())
                historyNow->whenNotCopying (
                    [&context, import]
                    {
                        auto* making = getVersionHistory (context);

                        if (making == nullptr)
                            return;

                        // Its audio only once nothing names it: the branch gone. One that can't go now (a commit being
                        // written) stays whole, with the audio it plays, which Clean Up can offer later; and is said.
                        if (making->deleteBranch (import->branch, true))
                        {
                            const std::scoped_lock held (import->lock);

                            for (const auto& file : import->audio.made)
                                file.deleteFile();
                        }
                        else
                        {
                            making->readingProblem = "The branch the import made couldn't be taken away again, as a version was being "
                                                     "written: it stays, with the audio it plays. Delete it from the branch picker.";
                            making->tellNow();
                        }
                    });

            if (done != nullptr)
                done (did, did ? import->said : juce::StringArray (historyNow != nullptr ? historyNow->readingProblem : juce::String()));
        },
        progress);
}
} // namespace phazite
