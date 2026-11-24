// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RandomEdits.h"
#include "TestClips.h"
#include "TestDeviceValues.h"
#include "devices/drums/DrumSampler.h"
#include "devices/drums/FactoryDrums.h"
#include "devices/dynamics/Compressor.h"
#include "devices/modulation/Modulators.h"
#include "model/arrangement/internal/Arrangement.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/SongState.h"
#include "model/chords/internal/Chords.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/devices/internal/SamplerSound.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/ContainerValues.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/presets/internal/Presets.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

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
                if (! midiOnly || isMidiClip (song, clip))
                    found.push_back (clip);

        return found;
    }

    std::vector<TrackID> getPlainTracks (const Song& song)
    {
        std::vector<TrackID> found;

        for (const auto track : getTrackIDs (song))
            if (! isFolder (song, track) && ! isReturnTrack (song, track))
                found.push_back (track);

        return found;
    }

    // The kinds of edit of notes, controllers' values and clips one by one (RandomEdits::editNotesOrClips()), and of those
    // that reach across the song's last beat, drawn far out alone (RandomEdits::editFarOut()).
    constexpr int numNoteAndClipKinds = 19;
    constexpr int numFarOutKinds = 9;

    // A song beat moved by an amount, at the song's start at the earliest.
    BeatPosition movedBy (BeatPosition beat, double beats)
    {
        return at (std::max (0.0, beat.inBeats() + beats));
    }
} // namespace

ClipID RandomEdits::pickMidiClip (bool withNotes)
{
    std::vector<ClipID> all, linked;

    for (const auto clip : getAllClips (song, true))
    {
        if (withNotes && getClipNotesPlayed (song, clip).empty())
            continue;

        all.push_back (clip);

        if (getClipValue (song, clip)->link != 0)
            linked.push_back (clip);
    }

    if (all.empty())
        return {};

    const auto ofAGroup = random.nextBool();
    return ofAGroup && ! linked.empty() ? pick (linked) : pick (all);
}

std::vector<NoteData> RandomEdits::pickNotes (ClipID clip)
{
    auto played = getClipNotesPlayed (song, clip);
    std::vector<NoteData> picked;
    const auto count = 1 + draw (3);

    for (int i = 0; i < count && ! played.empty(); ++i)
    {
        const auto index = (size_t) draw ((int) played.size());
        picked.push_back (played[index]);
        played.erase (played.begin() + (std::ptrdiff_t) index);
    }

    return picked;
}

void RandomEdits::editNotesOrClips (int kind)
{
    // The edits of notes and values, through a clip that holds notes that play.
    if (kind <= 11)
    {
        const auto clip = pickMidiClip (kind != 10 && kind != 11);

        if (! clip.isValid())
            return;

        const auto notes = pickNotes (clip);
        const auto refs = getRefs (notes);
        const auto range = getClipRange (song, clip);

        if (notes.empty() && kind <= 9)
            return;

        switch (kind)
        {
            case 0: // a note moved, as the piano roll's drag of one moves it
            {
                const auto by = (draw (17) - 8) * 0.25;
                const auto pitchBy = draw (7) - 3;
                moveNote (song, refs.front(), movedBy (notes.front().start, by), juce::jlimit (0, 127, notes.front().pitch + pitchBy));
                break;
            }

            case 1: // notes placed, as a step of a drag of several
            {
                const auto by = (draw (9) - 4) * 0.25;
                const auto pitchBy = draw (5) - 2;
                const auto longer = draw (3) * 0.25;
                auto placed = notes;

                for (auto& note : placed)
                {
                    note.start = movedBy (note.start, by);
                    note.pitch = juce::jlimit (0, 127, note.pitch + pitchBy);
                    note.length = note.length + lasting (longer);
                }

                placeNotes (song, placed);
                break;
            }

            case 2: // a note's length set, or the notes made longer or shorter
            {
                const auto which = draw (2);
                const auto beats = (draw (16) + 1) * 0.25;
                const auto by = (draw (9) - 4) * 0.25;

                if (which == 0)
                    setNoteLength (song, refs.front(), lasting (beats));
                else
                    changeNoteLengths (song, refs, lasting (by));
                break;
            }

            case 3:
                deleteNotes (song, refs);
                break;

            case 4:
            {
                const auto semitones = draw (25) - 12;
                transposeNotes (song, refs, semitones);
                break;
            }

            case 5:
            {
                const auto by = (draw (9) - 4) * 0.25;
                nudgeNotes (song, refs, lasting (by));
                break;
            }

            case 6: // muted, slid or portamento, or back
            {
                const auto which = draw (3);

                if (which == 0)
                    toggleNotesMuted (song, refs);
                else if (which == 1)
                    toggleNotesSlide (song, refs);
                else
                    toggleNotesPortamento (song, refs);
                break;
            }

            case 7: // velocities set or changed, or the channels set
            {
                const auto which = draw (3);
                const auto velocity = 1 + draw (127);
                const auto change = draw (41) - 20;
                const auto channel = 1 + draw (16);

                if (which == 0)
                    setNoteVelocities (song, refs, velocity);
                else if (which == 1)
                    changeNoteVelocities (song, refs, change);
                else
                    setNoteChannels (song, refs, channel);
                break;
            }

            case 8:
                duplicateNotes (song, refs);
                break;

            case 9: // replaced, as a quantize to the half beat (their IDs kept) or a chop into quarter beats replaces them
            {
                const auto chopped = random.nextBool();
                auto replacements = notes;

                if (chopped)
                {
                    replacements = chop (notes, 0.25);
                }
                else
                {
                    for (auto& note : replacements)
                        note.start = at (std::round (note.start.inBeats() * 2.0) / 2.0);
                }

                replaceNotes (song, refs, replacements);
                break;
            }

            case 10: // the clip's notes set whole, as the chord lane sets a part's
            {
                std::vector<NoteData> made;
                const auto count = draw (5);

                for (int i = 0; i < count; ++i)
                {
                    NoteData note;
                    note.clip = clip;
                    note.pitch = 36 + draw (48);
                    const auto along = draw (16) * 0.25;
                    note.start = range.getStart() + lasting (std::min (along, std::max (0.0, range.getLength().inBeats() - 0.25)));
                    note.length = lasting (0.25 + draw (8) * 0.25);
                    note.velocity = 1 + draw (127);
                    made.push_back (note);
                }

                setClipNotes (song, clip, made);
                break;
            }

            default: // controllers' values: added, recorded, removed, drawn or deleted, the mod wheel's and the bend's
            {
                const auto which = draw (6);
                const auto bend = random.nextBool();
                const ControlLane lane { bend ? ControlKind::pitchWheel : ControlKind::controller, bend ? 0 : 1, 1 };
                const auto length = range.getLength().inBeats();
                const auto first = random.nextDouble() * length;
                const auto second = random.nextDouble() * length;
                const auto value = draw (lane.getHighestValue() + 1);
                const auto otherValue = draw (lane.getHighestValue() + 1);
                const auto from = range.getStart() + lasting (std::min (first, second));
                const auto to = range.getStart() + lasting (std::max (first, second));

                if (which == 0)
                    addControl (song, clip, from, lane.kind, lane.number, value, lane.channel);
                else if (which == 1)
                    addControls (song, clip,
                                 { { from, lane.kind, lane.number, value, lane.channel },
                                   { to, lane.kind, lane.number, otherValue, lane.channel } });
                else if (which == 2)
                    removeControls (song, clip, lane.kind, lane.number, lane.channel, from, to);
                else if (which == 3)
                    setControlValues (song, clip, lane, { from, to }, { { {}, from, value, 0 }, { {}, to, otherValue, 0 } });
                else if (which == 4)
                    deleteControlValues (song, { clip }, lane, { from, to });
                else
                    deleteAllControlValues (song, { clip }, lane);
                break;
            }
        }

        return;
    }

    // The edits of clips one by one: a MIDI clip's, of a group half of the time, where there is one; or any clip's.
    const auto anyKind = random.nextBool();
    const auto clips = getAllClips (song, false);
    const auto clip = anyKind && ! clips.empty() ? pick (clips) : pickMidiClip (false);

    if (! clip.isValid())
        return;

    const auto range = getClipRange (song, clip);
    const auto along = random.nextDouble();

    switch (kind)
    {
        case 12:
        {
            const auto asAliases = random.nextBool();
            duplicateClips (song, { clip }, asAliases, nullptr);
            break;
        }

        case 13:
        {
            const auto closeGap = draw (4) == 0;
            deleteClips (song, { clip }, closeGap);
            break;
        }

        case 14:
            makeClipsUnique (song, { clip });
            break;

        case 15: // onto another track, at another beat
            if (const auto tracks = getPlainTracks (song); ! tracks.empty())
            {
                const auto track = pick (tracks);
                const auto beat = draw (32) * 0.5;
                moveClip (song, clip, drawnAt (beat), track);
            }
            break;

        case 16: // its start or its end trimmed, either way
        {
            const auto end = random.nextBool();
            const auto by = (along - 0.5) * range.getLength().inBeats();

            if (end)
                trimClipEnd (song, clip, movedBy (range.getEnd(), by));
            else
                trimClipStart (song, clip, movedBy (range.getStart(), by));
            break;
        }

        case 17:
            splitClips (song, { clip }, range.getStart() + lasting (range.getLength().inBeats() * along));
            break;

        default: // an alias of it, after it on its track
        {
            const auto copies = copyClips (song, { clip }, true);

            if (! copies.empty())
                moveClip (song, copies.front(), range.getEnd(), getClipTrack (song, clip));
            break;
        }
    }
}

void RandomEdits::editFarOut (int kind)
{
    // Each beat around the song's last beat, from where the far-out edits draw (farOut, a few beats before it) to as far past
    // it, through the model's functions as the editors call them.
    const auto lastBeat = schema::maxBeats;
    const auto plain = getPlainTracks (song);
    const auto beat = drawnAt (draw (40) * 0.5);

    switch (kind)
    {
        case 0: // a MIDI clip that ends at the last beat, a note placed past its end and made longer
            if (! plain.empty())
            {
                const auto on = pick (plain);
                const auto length = 1 + draw (8);
                const auto past = draw (8) * 0.5;
                const auto longer = (1 + draw (8)) * 0.5;

                if (const auto clip = addMidiClip (song, on, { at (lastBeat - length), at (lastBeat) }); clip.isValid())
                {
                    const auto note = addNote (song, clip, 60, at (lastBeat + past), lasting (0.5), 100);
                    setNoteLength (song, note, lasting (longer));
                }
            }
            break;

        case 1: // a controller's value, at a beat in a clip or past it
            if (const auto clip = pickMidiClip (false); clip.isValid())
            {
                const auto value = draw (128);
                addControl (song, clip, beat, ControlKind::controller, 1, value);
            }
            break;

        case 2: // an audio clip there, warped: a marker added and moved towards a beat across the last
            if (! plain.empty() && sound.existsAsFile())
            {
                const auto on = pick (plain);
                const auto along = random.nextDouble();
                const auto to = beat + lasting (draw (8) * 0.5);

                if (const auto clip = addAudioClip (song, on, "Sound", sound, toSeconds (song, beat), 0.5); clip.isValid())
                {
                    const auto time = 0.1 + along * 0.3;

                    if (addWarpMarker (song, clip, time))
                        if (const auto markers = getWarpMarkers (song, clip); ! markers.empty())
                        {
                            const auto marker = pick (markers);
                            moveWarpMarker (song, clip, marker.time, to);
                        }
                }
            }
            break;

        case 3: // a time signature there, after the song's others (at a beat none is at: two at one beat is known bug 240)
        {
            const auto numerator = 2 + draw (6);
            const auto eighths = random.nextBool();
            auto changes = getTimeSignatureChanges (song);
            const auto isThere = [beat] (const TimeSignatureChange& change) { return change.beat == beat; };

            if (std::none_of (changes.begin(), changes.end(), isThere))
            {
                changes.push_back ({ beat, { numerator, eighths ? 8 : 4 } });
                setTimeSignatureChanges (song, changes);
            }
            break;
        }

        case 4: // a clip's end trimmed across the last beat, either way
        case 5: // its start
            if (const auto clips = getAllClips (song, false); ! clips.empty())
            {
                const auto clip = pick (clips);

                if (kind == 4)
                    trimClipEnd (song, clip, beat);
                else
                    trimClipStart (song, clip, beat);
            }
            break;

        case 6: // a section made to end across the last beat, what comes after it moved along or not
            if (const auto sections = getSections (song); ! sections.empty())
            {
                const auto section = pick (sections);
                const auto moveRest = random.nextBool();
                const auto length = std::max (lasting (1.0), beat - section.range.getStart());
                resizeSection (song, section.id, length, moveRest);
            }
            break;

        case 7: // a chord made to end across the last beat
            if (const auto chords = getLaneChords (song); ! chords.empty())
                setLaneChordEnd (song, pick (chords).range.getStart(), beat);
            break;

        default: // time inserted before what is near the last beat
        {
            const auto length = (1 + draw (16)) * 0.5;
            insertTime (song, beat, lasting (length));
            break;
        }
    }
}

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
    const auto oneAreaKinds = editsOneAreaAlone ? numOneAreaKinds : 0;
    const auto farOutKinds = farOut > 0.0 ? numFarOutKinds : 0;
    const auto kind = draw (45 + numNoteAndClipKinds + oneAreaKinds + farOutKinds);

    // Where asked, after all the others, each kind's own after the other's: a seed's songs without them stay as they were.
    if (kind >= 45 + numNoteAndClipKinds + oneAreaKinds)
    {
        editFarOut (kind - 45 - numNoteAndClipKinds - oneAreaKinds);
        return;
    }

    if (kind >= 45 + numNoteAndClipKinds)
    {
        editOneArea (kind - 45 - numNoteAndClipKinds);
        return;
    }

    if (kind >= 45)
    {
        editNotesOrClips (kind - 45);
        return;
    }

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
            phazite::test::tryAddOwnDevice (song, track, info->typeID);
            break;
        }

        case 5: // one of its instruments, on a track that holds clips
            if (! plain.empty())
            {
                const auto instruments = getOwnOfKind (devices::DeviceKind::instrument);
                const auto* info = pick (instruments);
                phazite::test::trySetOwnInstrument (song, pick (plain), info->typeID);
            }
            break;

        case 6: // a value set by hand
            setAValueByHand();
            break;

        case 7: // a MIDI clip with a note
            if (! plain.empty())
            {
                const auto on = pick (plain);
                const auto start = draw (16);
                const auto length = 1 + draw (8);

                if (const auto clip = addMidiClip (song, on, { drawnAt (start), drawnAt (start + length) }); clip.isValid())
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
                    setClipFade (song, clip, true, fade); // at most 0.1 s of a clip of 0.5 s: as set
                }
            }
            break;

        case 11:                                                            // a track or folder deleted, while another stays
            if (tracks.size() > 1 + getTracksInFolder (song, track).size()) // itself and the tracks in it
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
                moveClip (song, moved, drawnAt (beat), other);
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

                if (const auto* info =
                        findOwnDevice (phazite::test::copyDevice (song, device)[schema::deviceType].toString().toStdString());
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
                point.beat = drawnAt (beat);
                point.position = along;
                addPoint (song, parameter, point);
            }
            break;
        }

        case 17: // a modulator, and what it modulates
        case 18: // a macro of the track's, and what it modulates
        {
            const auto modulator = kind == 17 ? addModulator (song, track, (ModulatorType) draw (6)) : addMacro (song, track, "Macro");
            const auto devices = getDevices (song, track);

            if (! modulator.isValid() || devices.empty())
                break;

            const auto device = pick (devices);
            const auto* info = findOwnDevice (phazite::test::copyDevice (song, device)[schema::deviceType].toString().toStdString());

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
                    phazite::test::tryAddOwnDeviceToChain (song, chains.front(), info->typeID);

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
            insertSection (song, drawnAt (start), section);
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
                putLaneChord (song, { drawnAt (start), drawnAt (start + 4) }, numeral);
            }
            else
            {
                setLaneKey (song, drawnAt (start), Key { degree, Scale::minor });
            }
            break;
        }

        case 22: // a tempo change, or the time signature
        {
            const auto beat = draw (16) * 4;
            const auto bpm = 60.0 + draw (120);

            if (random.nextInt (4) > 0)
                addTempoChange (song, drawnAt (beat), bpm);
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

                if (phazite::test::copyDevice (song, drums)[schema::deviceType].toString() != devices::DrumSampler::typeID)
                    drums = phazite::test::trySetOwnInstrument (song, on, devices::DrumSampler::typeID);

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

            if (phazite::test::copyDevice (song, device).hasType (schema::DEVICE) && device != getInstrument (song, track))
                moveToChain (song, device, chain);
            else if (const auto inside = getChainDevices (song, chain); inside.size() > 1)
                moveInChain (song, pick (inside), draw ((int) inside.size()));
            break;
        }

        case 34: // a factory preset loaded into a device, its values replaced as a whole (as Swap keeps one)
            if (const auto devices = getAllDevices (song); ! devices.empty())
            {
                const auto device = pick (devices);

                if (const auto* info =
                        findOwnDevice (phazite::test::copyDevice (song, device)[schema::deviceType].toString().toStdString()))
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
                const auto range = getClipRange (*getClipValue (song, clip)); // an audio clip's start alone, as its value's
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
                if (isAudioClip (song, clip))
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
            if (const auto curves = getCurveKeys (song); ! curves.empty())
            {
                const auto parameter = pick (curves);
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
                if (const auto frozen = getFrozenTracks (song); std::find (frozen.begin(), frozen.end(), track) != frozen.end())
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
                if ((juce::int64) phazite::test::copyDevice (song, device)[schema::deviceSidechain] != 0)
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

void RandomEdits::setAValueByHand()
{
    const auto all = getAllDevices (song);

    if (all.empty())
        return;

    const auto device = pick (all);
    const auto* info = findOwnDevice (phazite::test::copyDevice (song, device)[schema::deviceType].toString().toStdString());

    if (info == nullptr || info->parameters.empty())
        return;

    const auto& parameter = info->parameters[(size_t) draw ((int) info->parameters.size())];
    const auto along = random.nextFloat();
    const auto value = devices::constrain (parameter, parameter.minimum + along * (parameter.maximum - parameter.minimum));
    setValueSetByHand (song, { device, juce::String (parameter.id.data(), parameter.id.size()) }, value);
}

void RandomEdits::editOneArea (int kind)
{
    switch (kind)
    {
        case 0: // a send's level, of a send made before (the sends')
        {
            std::vector<DeviceID> sends;

            for (const auto track : getTrackIDs (song))
                for (const auto send : getSends (song, track))
                    sends.push_back (send);

            if (sends.empty())
                break;

            const auto send = pick (sends);
            const auto level = -random.nextFloat() * 30.0f;
            setSendLevel (song, send, level);
            break;
        }

        case 1: // a container's macro renamed, its value moved, or a device in its chains modulated by it (modulation's)
        case 2:
        case 3:
        {
            const auto containers = getAllContainers (song);

            if (containers.empty())
                break;

            const auto container = pick (containers);
            const auto macros = getContainerMacros (song, container);

            if (macros.empty())
                break;

            const auto macro = pick (macros);

            if (kind == 1)
            {
                const auto number = draw (100);
                renameMacro (song, macro, "Macro " + juce::String (number));
                break;
            }

            if (kind == 2)
            {
                const auto value = random.nextFloat();
                setMacroValue (song, macro, value);
                break;
            }

            const auto inside = getContainerDevices (song, container);

            if (inside.empty())
                break;

            const auto device = pick (inside);
            const auto* info = findOwnDevice (phazite::test::copyDevice (song, device)[schema::deviceType].toString().toStdString());

            if (info == nullptr || info->parameters.empty())
                break;

            const auto& p = info->parameters[(size_t) draw ((int) info->parameters.size())];
            const ParameterRef parameter { device, juce::String (p.id.data(), p.id.size()) };
            const auto amount = random.nextFloat() * 2.0f - 1.0f;

            if (canModulate (song, macro, parameter))
                setModulation (song, macro, parameter, amount);
            break;
        }

        case 4: // a modulator's parameter, its rate where it has one (modulation's)
        {
            std::vector<ModulatorID> modulators;

            for (const auto track : getTrackIDs (song))
                for (const auto modulator : getModulators (song, track))
                    if (! isMacro (song, modulator))
                        modulators.push_back (modulator);

            if (modulators.empty())
                break;

            const auto modulator = pick (modulators);
            const auto value = getModulatorValue (song, modulator);

            if (! value.has_value() || ! value->kind.has_value())
                break;

            const auto parameters = devices::getModulatorParameters (*value->kind);

            if (parameters.empty())
                break;

            const auto rate = devices::findModulatorParameter (*value->kind, "rate");
            const auto index = rate >= 0 ? rate : draw ((int) parameters.size());
            const auto& parameter = parameters[(size_t) index];
            const auto along = random.nextFloat();
            setModulatorValueSetByHand (song, modulator, parameter, parameter.minimum + along * (parameter.maximum - parameter.minimum));
            break;
        }

        case 5: // a device's value set by hand (the devices')
            setAValueByHand();
            break;

        case 6: // a container's chain muted or soloed, or back (the devices' store)
            if (const auto containers = getAllContainers (song); ! containers.empty())
            {
                const auto chains = getChains (song, pick (containers));

                if (chains.empty())
                    break;

                const auto chain = pick (chains);

                if (random.nextBool())
                    setChainMuted (song, chain, ! isChainMuted (song, chain));
                else
                    setChainSoloed (song, chain, ! isChainSoloed (song, chain));
            }
            break;

        case 7: // a track muted, or not (the tracks' switches)
            if (const auto tracks = getTrackIDs (song); ! tracks.empty())
            {
                const auto track = pick (tracks);
                setTrackMuted (song, track, ! isTrackMuted (song, track, false));
            }
            break;

        case 8: // an automation point moved (the curves')
            if (const auto curves = getCurveKeys (song); ! curves.empty())
            {
                const auto parameter = pick (curves);
                const auto points = getPoints (song, parameter);

                if (points.empty())
                    break;

                const auto index = draw ((int) points.size());
                const auto by = draw (5) - 2;
                const auto change = (random.nextFloat() - 0.5f) * 0.2f;
                movePoints (song, parameter, { index }, lasting (by), change);
            }
            break;

        case 9: // a section renamed (the sections')
            if (const auto sections = getSections (song); ! sections.empty())
            {
                const auto section = pick (sections);
                const auto number = draw (100);
                renameSection (song, section.id, "Part " + juce::String (number));
            }
            break;

        case 10: // a tempo change (the tempo map's)
        {
            const auto beat = draw (16) * 4;
            const auto bpm = 60.0 + draw (120);
            addTempoChange (song, at (beat), bpm);
            break;
        }

        case 11: // a chord on the chord lane (the chord lane's)
        {
            const auto start = draw (8) * 4;
            Numeral numeral;
            numeral.degree = 1 + draw (7);
            putLaneChord (song, { at (start), at (start + 4) }, numeral);
            break;
        }

        case 12: // the key set last, which new tracks get (the song's standing)
        {
            const auto root = draw (12);
            const auto minor = random.nextBool();
            setLastKey (song, Key { root, minor ? Scale::minor : Scale::major });
            break;
        }

        case 13: // the song's MIDILINKS alone, put in or taken away empty, as a version's links give it (the root's places)
            if (getMidiLinks (song).empty())
            {
                const auto placed = getSongState (song).holdsPlace (RootPlace::midiLinks);
                makeMidiLinksAs (song, placed ? juce::ValueTree() : juce::ValueTree (schema::MIDILINKS));
            }
            break;

        case 14: // a controller linked to a track's volume (the MIDI links')
            if (const auto tracks = getTrackIDs (song); ! tracks.empty())
            {
                const auto track = pick (tracks);
                const auto number = 20 + draw (8);
                linkMidiController (song, { 1, number }, { getFader (song, track), getVolumeParameterID (song, track) });
            }
            break;

        case 15: // the project's notes (the song's own)
        {
            const auto number = draw (100);
            auto own = getSongOwn (song);
            own.notes = "Notes " + juce::String (number);
            setSongOwn (song, std::move (own));
            break;
        }

        case 16: // a note added to a MIDI clip (the clips' notes)
            if (const auto clips = getAllClips (song, true); ! clips.empty())
            {
                const auto clip = pick (clips);
                const auto pitch = 30 + draw (40);
                const auto beat = draw (4);
                addMidiNote (song, clip, pitch, at (beat), lasting (0.25));
            }
            break;

        case 17: // a clip muted, or not (the clips')
            if (const auto clips = getAllClips (song, false); ! clips.empty())
                toggleClipsMuted (song, { pick (clips) });
            break;

        default: // a track renamed (the tracks')
            if (const auto tracks = getTrackIDs (song); ! tracks.empty())
            {
                const auto track = pick (tracks);
                const auto number = draw (100);
                setTrackName (song, track, "Track " + juce::String (number));
            }
            break;
    }
}

void RandomEdits::addOneOfEach()
{
    const auto plain = getPlainTracks (song);
    const auto track = plain.empty() ? insertTrack (song, {}, nullptr) : plain.front();
    const auto* effect = getOwnOfKind (devices::DeviceKind::effect).front();
    phazite::test::tryAddOwnDevice (song, track, effect->typeID);

    if (const auto container = addContainer (song, track); container.isValid())
        if (const auto chains = getChains (song, container); ! chains.empty())
            phazite::test::tryAddOwnDeviceToChain (song, chains.front(), effect->typeID);

    addModulator (song, track, ModulatorType::lfo);
    addSend (song, track, addReturnTrack (song, "Return", nullptr), false);

    if (const auto clip = addMidiClip (song, track, { at (0.0), at (4.0) }); clip.isValid())
        addMidiNote (song, clip, 60, at (0.0), lasting (0.5));

    SectionInfo section;
    section.name = "Part";
    section.length = lasting (8.0);
    insertSection (song, at (0.0), section);

    Numeral numeral;
    numeral.degree = 1;
    putLaneChord (song, { at (0.0), at (4.0) }, numeral);

    // No MIDI link: the first is made by the edits of one area (editOneArea()), after the place they are kept in.
    const ParameterRef volume { getFader (song, track), getVolumeParameterID (song, track) };
    AutomationPoint point;
    point.beat = at (0.0);
    point.position = 0.5f;
    addPoint (song, volume, point);
}
} // namespace phazite::test
