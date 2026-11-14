// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RandomEdits.h"
#include "TestClips.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/chords/internal/Chords.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/transport/internal/Tempo.h"

namespace phazite::test
{
namespace
{
    std::vector<const devices::DeviceInfo*> getOwnOfKind (devices::DeviceKind kind)
    {
        std::vector<const devices::DeviceInfo*> found;

        for (const auto* info : getOwnDevices())
            if (info->kind == kind)
                found.push_back (info);

        return found;
    }

    // Every device of the song's tracks and the master, those in containers too.
    std::vector<DeviceID> getAllDevices (const Song& song)
    {
        std::vector<DeviceID> found;
        auto channels = getTrackIDs (song);
        channels.push_back ({}); // the master

        for (const auto channel : channels)
        {
            for (const auto device : getDevices (song, channel))
            {
                found.push_back (device);

                for (const auto inside : getContainerDevices (song, device))
                    found.push_back (inside);
            }
        }

        return found;
    }

    std::vector<ClipID> getAllClips (const Song& song, bool midiOnly)
    {
        std::vector<ClipID> found;

        for (const auto track : getTrackIDs (song))
            for (const auto clip : getClips (song, track))
                if (! midiOnly || findClipState (song, clip)[schema::clipKind].toString() == schema::clipIsMidi)
                    found.push_back (clip);

        return found;
    }

    std::vector<TrackID> getPlainTracks (const Song& song)
    {
        std::vector<TrackID> found;

        for (const auto track : getTrackIDs (song))
            if (findTrackState (song, track)[schema::trackKind].toString() == schema::trackIsTrack)
                found.push_back (track);

        return found;
    }

    size_t countTracks (const juce::ValueTree& track)
    {
        size_t tracks = 1;

        for (const auto& child : track)
            if (child.hasType (schema::TRACK))
                tracks += countTracks (child);

        return tracks;
    }
} // namespace

RandomEdits::RandomEdits (Song& s, juce::int64 seed, const juce::File& soundsFolder)
    : random (seed),
      song (s)
{
    if (soundsFolder != juce::File())
    {
        sound = soundsFolder.getChildFile ("Random edits.wav");

        if (! sound.existsAsFile())
            writeSound (sound, 1.0);
    }
}

void RandomEdits::edit (int count)
{
    for (int i = 0; i < count; ++i)
        edit();
}

void RandomEdits::edit()
{
    auto tracks = getTrackIDs (song);

    if (tracks.empty()) // its last folder ungrouped with nothing in it
    {
        insertTrack (song, {}, nullptr);
        return;
    }

    const auto track = pick (tracks);
    const auto other = pick (tracks);
    const auto plain = getPlainTracks (song);
    const auto kind = draw (30);

    switch (kind)
    {
        case 0:
        {
            const auto decibels = -random.nextFloat() * 30.0f;
            setChannelVolume (song, track, decibels);
            break;
        }

        case 1:
        {
            const auto pan = random.nextFloat() * 2.0f - 1.0f;
            setChannelPan (song, track, pan);
            break;
        }

        case 2:
            insertTrack (song, track, nullptr);
            break;

        case 3: // a return, and a send to it
        {
            const auto returnTrack = addReturnTrack (song, "Return", nullptr);
            const auto preFader = random.nextBool();
            const auto level = -random.nextFloat() * 20.0f;

            if (const auto send = addSend (song, track, returnTrack, preFader); send.isValid())
                setSendLevel (song, send, level);
            break;
        }

        case 4: // one of Phazite's own effects
        {
            const auto effects = getOwnOfKind (devices::DeviceKind::effect);
            const auto* info = pick (effects);
            insertDeviceState (song, track, makeOwnDeviceState (song, info->typeID));
            break;
        }

        case 5: // one of its instruments, on a track that holds clips
            if (! plain.empty())
            {
                const auto instruments = getOwnOfKind (devices::DeviceKind::instrument);
                const auto* info = pick (instruments);
                setInstrument (song, pick (plain), makeOwnDeviceState (song, info->typeID));
            }
            break;

        case 6: // a value set by hand
        {
            const auto devices = getAllDevices (song);

            if (devices.empty())
                break;

            const auto device = pick (devices);
            const auto* info = findOwnDevice (findDeviceState (song, device)[schema::deviceType].toString().toStdString());

            if (info == nullptr || info->parameters.empty())
                break;

            const auto& parameter = info->parameters[(size_t) draw ((int) info->parameters.size())];
            const auto along = random.nextFloat();
            const auto value = devices::constrain (parameter, parameter.minimum + along * (parameter.maximum - parameter.minimum));
            setValueSetByHand (song, { device, juce::String (parameter.id.data(), parameter.id.size()) }, value);
            break;
        }

        case 7: // a MIDI clip with a note
            if (! plain.empty())
            {
                const auto on = pick (plain);
                const auto start = draw (16);
                const auto length = 1 + draw (8);

                if (const auto clip = addMidiClip (song, on, { at (start), at (start + length) }); clip.isValid())
                {
                    const auto pitch = 40 + draw (20);
                    addMidiNote (song, clip, pitch, at (0.0), lasting (0.5));
                }
            }
            break;

        case 8: // a note in a clip
        case 9:
            if (const auto clips = getAllClips (song, true); ! clips.empty())
            {
                const auto clip = pick (clips);
                const auto pitch = 30 + draw (40);
                const auto beat = draw (8);
                const auto velocity = 1 + draw (127);
                addMidiNote (song, clip, pitch, at (beat), lasting (0.25), velocity);
            }
            break;

        case 10: // an audio clip, with its gain, pitch and fades
            if (! plain.empty() && sound.existsAsFile())
            {
                const auto on = pick (plain);
                const auto start = random.nextDouble() * 4.0;

                if (const auto clip = addAudioClip (song, on, "Sound", sound, start, 0.5); clip.isValid())
                {
                    const auto gain = random.nextFloat() * 12.0f - 6.0f;
                    const auto pitch = (float) (draw (13) - 6);
                    const auto fade = random.nextDouble() * 0.1;
                    setClipGain (song, clip, gain);
                    setClipPitch (song, clip, pitch);
                    setClipFade (song, findClipState (song, clip), true, fade);
                }
            }
            break;

        case 11: // a track or folder deleted, while another stays
            if (tracks.size() > countTracks (findTrackState (song, track)))
                deleteTracks (song, { track }, nullptr);
            break;

        case 12:
            groupIntoFolder (song, track == other ? std::vector { track } : std::vector { track, other }, nullptr);
            break;

        case 13:
            ungroupFolder (song, track); // if it is a folder
            break;

        case 14: // a clip onto another track, at another beat
            if (const auto clips = getClips (song, track); ! clips.empty() && ! isFolder (song, other))
            {
                const auto moved = pick (clips);
                const auto beat = draw (16);
                moveClip (song, moved, at (beat), other);
            }
            break;

        case 15: // a copy of a clip, or an alias of it
            if (const auto clips = getAllClips (song, false); ! clips.empty())
            {
                const auto clip = pick (clips);
                const auto asAlias = random.nextBool();
                copyClips (song, { clip }, asAlias);
            }
            break;

        case 16: // automation: a device's parameter, or the track's volume
        {
            const auto devices = getAllDevices (song);
            ParameterRef parameter { getFader (song, track), getVolumeParameterID (song, track) };

            if (! devices.empty() && random.nextBool())
            {
                const auto device = pick (devices);

                if (const auto* info = findOwnDevice (findDeviceState (song, device)[schema::deviceType].toString().toStdString());
                    info != nullptr && ! info->parameters.empty())
                {
                    const auto& p = info->parameters[(size_t) draw ((int) info->parameters.size())];
                    parameter = { device, juce::String (p.id.data(), p.id.size()) };
                }
            }

            if (const auto description = describeParameter (song, parameter))
            {
                const auto beat = draw (32) * 0.5;
                const auto along = random.nextFloat();
                AutomationPoint point;
                point.beat = at (beat);
                point.value = description->range.convertFrom0to1 (along);
                addPoint (song, parameter, point);
            }
            break;
        }

        case 17: // a modulator, and what it modulates
        case 18: // a macro of the track's, and what it modulates
        {
            const auto modulator =
                kind == 17 ? addModulator (song, track, (ModulatorType) draw (6)) : addMacro (song, findTrackState (song, track), "Macro");
            const auto devices = getDevices (song, track);

            if (! modulator.isValid() || devices.empty())
                break;

            const auto device = pick (devices);
            const auto* info = findOwnDevice (findDeviceState (song, device)[schema::deviceType].toString().toStdString());

            if (info == nullptr || info->parameters.empty())
                break;

            const auto& p = info->parameters[(size_t) draw ((int) info->parameters.size())];
            const ParameterRef parameter { device, juce::String (p.id.data(), p.id.size()) };
            const auto amount = random.nextFloat() * 2.0f - 1.0f;

            if (canModulate (song, modulator, parameter))
                setModulation (song, modulator, parameter, amount);
            break;
        }

        case 19: // a container with an effect in a chain, and a second chain
        {
            if (const auto container = addContainer (song, track); container.isValid())
            {
                const auto effects = getOwnOfKind (devices::DeviceKind::effect);
                const auto* info = pick (effects);

                if (const auto chains = getChains (song, container); ! chains.empty())
                    insertIntoChain (song, chains.front(), makeOwnDeviceState (song, info->typeID));

                if (random.nextBool())
                    addChain (song, container);
            }
            break;
        }

        case 20: // a section
        {
            const auto start = draw (8) * 16;
            SectionInfo section;
            section.name = "Part";
            section.length = lasting (8 + draw (3) * 8);
            insertSection (song, at (start), section);
            break;
        }

        case 21: // a chord, or a key on the chord lane
        {
            const auto start = draw (8) * 4;
            const auto degree = 1 + draw (7);

            if (random.nextBool())
            {
                Numeral numeral;
                numeral.degree = degree;
                putLaneChord (song, { at (start), at (start + 4) }, numeral);
            }
            else
            {
                setLaneKey (song, at (start), Key { degree, Scale::minor });
            }
            break;
        }

        case 22: // a tempo change, or the time signature
        {
            const auto beat = draw (16) * 4;
            const auto bpm = 60.0 + draw (120);

            if (random.nextInt (4) > 0)
                addTempoChange (song, at (beat), bpm);
            else
                setTimeSignature (song, { 2 + draw (6), random.nextBool() ? 4 : 8 });
            break;
        }

        case 23: // a sidechain: the track ducked under another
            if (track != other && ! isFolder (song, track) && ! isFolder (song, other))
                duckUnder (song, track, other);
            break;

        case 24: // a drum sampler's pad on a track of its own
            if (! plain.empty())
            {
                const auto on = pick (plain);
                auto drums = getInstrument (song, on);

                if (findDeviceState (song, drums)[schema::deviceType].toString() != devices::DrumSampler::typeID)
                    drums = setInstrument (song, on, makeOwnDeviceState (song, devices::DrumSampler::typeID));

                const auto pad = 36 + draw (4);

                if (drums.isValid())
                    givePadItsOwnTrack (song, drums, pad);
            }
            break;

        case 25: // a folder made a bus
            if (isFolder (song, track))
                setMixedAsBus (song, track, random.nextBool());
            break;

        case 26:
            setTrackMuted (song, track, random.nextBool());
            break;

        case 27:
        {
            const auto number = draw (100);
            setTrackName (song, track, "Track " + juce::String (number));
            break;
        }

        case 28:
        {
            const auto root = draw (12);
            setTrackKey (song, track, Key { root, Scale::major });
            break;
        }

        default:
        {
            const auto colour = (juce::uint32) random.nextInt();
            setTrackColour (song, track, juce::Colour (colour | 0xff000000u));
            break;
        }
    }
}
} // namespace phazite::test
