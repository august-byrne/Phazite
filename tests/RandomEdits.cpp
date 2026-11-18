// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RandomEdits.h"
#include "TestClips.h"
#include "devices/drums/DrumSampler.h"
#include "devices/drums/FactoryDrums.h"
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
#include "model/devices/internal/SamplerSound.h"
#include "model/mixer/ContainerValues.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/presets/internal/Presets.h"
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

    // Every container of the song's tracks and the master.
    std::vector<DeviceID> getAllContainers (const Song& song)
    {
        std::vector<DeviceID> found;

        for (const auto device : getAllDevices (song))
            if (isContainer (song, device))
                found.push_back (device);

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
    const auto kind = draw (45);

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
                    // Most at their own pitch, which plays the file itself; the others pitched, which plays a render of it
                    // (renderMissingClipAudio(), where a test makes them).
                    const auto pitched = draw (4) == 0;
                    const auto pitch = pitched ? (float) (draw (13) - 6) : 0.0f;
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
        {
            // Built without a braced list in each branch of a ?:, which GCC's optimised builds misread as dangling.
            auto grouped = std::vector { track };

            if (other != track)
                grouped.push_back (other);

            groupIntoFolder (song, grouped, nullptr);
            break;
        }

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
                point.position = along;
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

        case 30: // a device switched off, or on again
            if (const auto devices = getAllDevices (song); ! devices.empty())
            {
                const auto device = pick (devices);
                const auto on = random.nextBool();
                setDeviceOn (song, device, on);
            }
            break;

        case 31: // a device deleted
            if (const auto devices = getAllDevices (song); ! devices.empty())
                removeDevice (song, pick (devices));
            break;

        case 32: // a track's device moved in its chain
            if (const auto devices = getDevices (song, track); devices.size() > 1)
            {
                const auto device = pick (devices);
                const auto index = draw ((int) devices.size());
                moveDevice (song, device, index);
            }
            break;

        case 33: // a device moved into a container's chain, or within one
        {
            const auto devices = getDevices (song, track);
            const auto containers = getAllContainers (song);

            if (containers.empty() || devices.empty())
                break;

            const auto chains = getChains (song, pick (containers));

            if (chains.empty())
                break;

            const auto chain = pick (chains);
            const auto device = pick (devices);

            if (findDeviceState (song, device).hasType (schema::DEVICE) && device != getInstrument (song, track))
                moveToChain (song, device, chain);
            else if (const auto inside = getChainDevices (song, chain); inside.size() > 1)
                moveInChain (song, pick (inside), draw ((int) inside.size()));
            break;
        }

        case 34: // a factory preset loaded into a device, its values replaced as a whole (as Swap keeps one)
            if (const auto devices = getAllDevices (song); ! devices.empty())
            {
                const auto device = pick (devices);

                if (const auto* info = findOwnDevice (findDeviceState (song, device)[schema::deviceType].toString().toStdString()))
                {
                    const auto presets = getFactoryPresetsFolder()
                                             .getChildFile (juce::String (info->name.data(), info->name.size()))
                                             .findChildFiles (juce::File::findFiles, true, "*.phazitepreset");

                    if (! presets.isEmpty())
                        tryPresetIn (song, device, presets[draw (presets.size())]);
                }
            }
            break;

        case 35: // a drum sampler's pad, or a sampler, given another of its factory sounds
            for (const auto device : getAllDevices (song))
            {
                if (isDrumSampler (song, device))
                {
                    const auto names = devices::getFactoryDrumNames();
                    const auto name = names[(size_t) draw ((int) names.size())];
                    const auto pad = 36 + draw (8);
                    setDeviceSound (song, device, schema::getPadSound (pad, 0), juce::String (name.data(), name.size()));
                    break;
                }

                if (isSampler (song, device))
                {
                    setFactorySample (song, device, draw (4));
                    break;
                }
            }
            break;

        case 36: // a container's chain muted, soloed, given its keys or its crossover
            if (const auto containers = getAllContainers (song); ! containers.empty())
            {
                const auto chains = getChains (song, pick (containers));

                if (chains.empty())
                    break;

                const auto chain = pick (chains);
                const auto which = draw (4);
                const auto low = draw (64);
                const auto span = 12 + draw (48);

                if (which == 0)
                    setChainMuted (song, chain, random.nextBool());
                else if (which == 1)
                    setChainSoloed (song, chain, random.nextBool());
                else if (which == 2)
                    setChainKeys (song, chain, { low, low + span });
                else
                    setChainCrossover (song, chain, 100.0f + (float) draw (5000));
            }
            break;

        case 37: // a container split otherwise, or its mix
            if (const auto containers = getAllContainers (song); ! containers.empty())
            {
                const auto container = pick (containers);

                if (random.nextBool())
                    setContainerSplit (song, container, (ContainerSplit) draw (4));
                else
                    setMix (song, container, random.nextFloat());
            }
            break;

        case 38: // a modulation's amount changed, or the modulation taken away
            for (const auto modulator : getModulators (song, track))
            {
                if (const auto modulations = getModulations (song, modulator); ! modulations.empty())
                {
                    const auto modulation = pick (modulations);

                    if (random.nextBool())
                    {
                        const auto amount = random.nextFloat() * 2.0f - 1.0f;
                        setModulation (song, modulator, modulation.parameter, amount);
                    }
                    else
                    {
                        removeModulation (song, modulator, modulation.parameter);
                    }

                    break;
                }
            }
            break;

        case 39: // a clip muted, resized or split
            if (const auto clips = getAllClips (song, false); ! clips.empty())
            {
                const auto clip = pick (clips);
                const auto range = getClipRange (findClipState (song, clip));
                const auto which = draw (3);
                const auto along = random.nextDouble();

                if (which == 0)
                    toggleClipsMuted (song, { clip });
                else if (which == 1)
                    trimClipEnd (song, clip, range.getStart() + lasting (0.25 + along * 4.0));
                else
                    splitClips (song, { clip }, range.getStart() + lasting (range.getLength().inBeats() * along));
            }
            break;

        case 40: // an audio clip warped, played backwards, or at another tempo of its own
        {
            std::vector<ClipID> audio;

            for (const auto clip : getAllClips (song, false))
                if (isAudioState (findClipState (song, clip)))
                    audio.push_back (clip);

            if (audio.empty())
                break;

            const auto clip = pick (audio);
            const auto which = draw (3);
            const auto along = random.nextDouble();

            if (which == 0)
            {
                setClipFollowsTempo (song, clip, true);
                addWarpMarker (song, clip, 0.1 + along * 0.3);
            }
            else if (which == 1)
            {
                setClipReversed (song, clip, along < 0.5);
            }
            else
            {
                setClipTempo (song, clip, 90.0 + along * 60.0);
            }
            break;
        }

        case 41: // an automation point moved or taken away
            if (const auto curves = getCurveStates (song); ! curves.empty())
            {
                const auto parameter = getCurveParameter (pick (curves));
                const auto points = getPoints (song, parameter);

                if (points.empty())
                    break;

                const auto index = draw ((int) points.size());
                const auto by = draw (5) - 2;
                const auto change = (random.nextFloat() - 0.5f) * 0.2f;

                if (random.nextBool())
                    movePoints (song, parameter, { index }, lasting (by), change);
                else
                    deletePoints (song, parameter, { index });
            }
            break;

        case 42: // a track frozen from a file, or unfrozen (where asked: freezes)
            if (freezes && song.getCacheFolder() != juce::File() && sound.existsAsFile() && ! isFolder (song, track))
            {
                if ((bool) findTrackState (song, track)[schema::trackFrozen])
                {
                    unfreeze (song, track);
                }
                else
                {
                    song.getCacheFolder().createDirectory();
                    const auto rendered = song.getCacheFolder().getNonexistentChildFile ("Rendered", ".wav");

                    if (sound.copyFileTo (rendered))
                        freezeFromFile (song, track, rendered, &song.getUndoManager());
                }
            }
            break;

        case 43: // a sidechain taken away
            for (const auto device : getAllDevices (song))
            {
                if ((juce::int64) findDeviceState (song, device)[schema::deviceSidechain] != 0)
                {
                    setSidechainSource (song, device, {});
                    break;
                }
            }
            break;

        case 44: // a send's level changed after it was made
            if (const auto sends = getSends (song, track); ! sends.empty())
            {
                const auto send = pick (sends);
                const auto level = -random.nextFloat() * 30.0f;
                setSendLevel (song, send, level);
            }
            break;

        default:
        {
            const auto colour = (juce::uint32) random.nextInt();
            setTrackColour (song, track, juce::Colour (colour | 0xff000000u));
            break;
        }
    }
}
} // namespace phazite::test
