// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's sends as values (roadmap step S5.20, the sends' area): a send as a value (SendValue), every send of a song
// (SongSends) with the helpers that alone write it, and the conversion between a SEND element and the value (Schema.h), which
// the sends' readers read a send with. The song's tree still holds its sends (the sends' first piece: their values
// and their readers); the sends' code (model/mixer/internal/Mixer.h: getSendValue(), getSendValues(), getSendTrack()) reads
// them as these values, from the tree for now, from an area of the song's values (SongValues.h) once the sends leave it.
// Until then other code still names SEND: the sends' writers (model/mixer/Mixer.cpp: addSend(), setSendLevel(),
// setPreFader(), removeSend(), forgetWhatNamesTheGone()) and the loader (model/project/Project.cpp), until the switch; the
// keeper's feed of what hears a send change (SendsArea.h: SongValues::SendsKeeper, which tells Listener::sendsChanged() from
// the tree's notifications, and the unsaved check's comparison by element), until the switch too; the compile's modulations
// (SongPlayer::addModulation()), until modulation's pieces; the versions' difference and merge, which compare whole copies
// (model/versions/SongDiff.cpp, SongMerge.cpp); and the selection's walk for a curve's owner, which skips a SEND
// (model/base/Selection.cpp).
//
// Its own header, beside SongValues.h and the other areas' (one header for each area's values). The song's reader of whether
// it holds a send is here, in the model's base, rather than with the sends' code, because the selection (Selection.h), which
// the base holds, asks whether a curve's owner is there, a send among them, and the base includes no other area. immer is the
// model's alone (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/Immer.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace phazite
{
class Song;

// A send as the song holds it (Schema.h: SEND): its ID (a DeviceID, by which curves, modulations, MIDI links and controls
// name its level: Schema.h, sendLevelParameter), the track it is on, the return it sends to (by the return's track's ID),
// its level in decibels (-100 for silence, 0 where its element has none) and whether it sends before its track's fader.
struct SendValue
{
    DeviceID id {};        // sendID
    TrackID from {};       // the TRACK it is on: none for a send on none (a copy)
    TrackID to {};         // sendTo
    float level = 0.0f;    // sendLevel, in decibels, -100 to 6
    bool preFader = false; // sendPreFader

    // Written out, not defaulted: a defaulted one would compare the level with a floating-point ==, which GCC's -Wfloat-equal
    // refuses. The level compared exactly, as it is saved.
    bool operator== (const SendValue&) const noexcept;
};

// Every send of a song: the sends by their IDs, and each track's sends in their order (the order they were added in, which is
// the order its element holds them in; a track without sends has no entry). The helpers below are the only writers of the
// two, and keep them in step: holdsItsLists() says whether they are.
struct SongSends
{
    immer::map<std::uint64_t, SendValue> sends {};
    immer::map<std::uint64_t, immer::flex_vector<DeviceID>> tracks {};

    // A send by its ID, or none. A pointer into this value, valid while it lives: keep the value in a local of its own, never
    // a temporary's.
    const SendValue* find (DeviceID id) const { return sends.find (id.getRaw()); }

    // The same two without a look inside: what an area compares (ValueArea.h). And the same sends in the same orders.
    bool isSameAs (const SongSends& other) const noexcept
    {
        return sends.identity() == other.sends.identity() && tracks.identity() == other.tracks.identity();
    }

    bool operator== (const SongSends&) const;
};

// A send added, at a place in its track's list (from 0, or -1 for after the others there), or one of them changed: on another
// track, it leaves its old track's list for the new one's (at the place given, or at its end). A send that stays on its track
// keeps its place there.
SongSends withSend (const SongSends&, SendValue, int index = -1);

// A send taken out of its track's list (nothing for one that isn't there).
SongSends withoutSend (const SongSends&, DeviceID);

// A track's sends put in another order: the same sends (else nothing changes).
SongSends withTrackSendOrder (const SongSends&, TrackID, immer::flex_vector<DeviceID> order);

// Whether the two agree: each send once in its track's list, under its own ID, each list holding sends of that track alone,
// none of them empty, and a send on none in none. For the tests, and the assertions where it is cheap.
bool holdsItsLists (const SongSends&);

//==============================================================================
// The conversion between a send's element and its value, which the sends' readers read a send with (what else still names
// SEND, and until when: the top of this header).

// A send's value read from its SEND element, each property as the sends' code has always read it (setSendProperty()): its
// ID and its return as readID() reads one, a number or text (its return written as a number: Schema.h, sendTo, which no one
// is to make text); its level by its description (SongText.h: readDescribed(), within -100 to 6, 0 where it has none or no
// finite number); before its fader where its element says so. A property Phazite doesn't know is no part of it. Its track is
// the one given (the TRACK its element is on). What reads the sends of a song whose tree holds them (Mixer.h:
// getSendValue(), getSendValues()), and the tests.
SendValue readSendValue (const juce::ValueTree& sendElement, TrackID from);

// How many sends were read into values (readSendValue(), which every reader below reads each send with) since Phazite
// started, on any thread: for the test that a tick of a send's level control reads as many whatever the song's size
// (tests/PerformanceGateTests.cpp). Allocations can't tell it: a value read makes none.
std::uint64_t countSendsRead() noexcept;

// A send's value read so from its SEND element as the song holds it (the sends' area, once they leave the tree: what the
// song takes in as it opens, the keeper's safety net, a version's sends and a merge's choice): as readSendValue() reads it;
// none for an element that is no SEND, and for one without an ID the song can count on from (0, or at or past
// schema::maxID), which no send holds.
std::optional<SendValue> readHeldSend (const juce::ValueTree& sendElement, TrackID from);

// A send as a SEND element, as the sends' code writes one (model/mixer/Mixer.cpp: addSend(), setSendLevel(), setPreFader()):
// its ID and its return always, as numbers; its level while it isn't 0 dB, the float it is (written as the fewest digits that
// read back as it: what setSendLevel() wrote, to a ten-thousandth, writes the same text again); and preFader only while it
// sends before the fader. Its track is where the element goes.
juce::ValueTree writeSendElement (const SendValue&);

// A TRACK element's sends, in the order the element holds them, each as readSendValue() reads it (none for an element that
// is no TRACK); and its send of an ID, or none (the first with it). What reads a track's sends of a song whose tree holds
// them (Mixer.h: getSendValues(), getSendValue()), a control's parameter that found its send on the track (Parameters.h:
// FoundParameter) and a count of every SEND the tracks hold (model/versions/ApplySong.cpp: removeBrokenReferences()); a walk
// of the element's children, nothing else.
std::vector<SendValue> readTrackSends (const juce::ValueTree& trackElement);
std::optional<SendValue> findTrackSend (const juce::ValueTree& trackElement, DeviceID);

// The song's SEND elements its sends are read from (readSendsValue()), from its tree (a PHAZITE element, or a whole copy of
// one): each TRACK of its track list with an ID (a folder before the tracks in it), with that ID as held, and its SEND
// elements with an ID the song holds a send by (readHeldSend()), each the first of its ID in the song, in the track's order
// (a track's list, empty for one with none, valid during the call). A track without an ID holds none of the song's sends and
// hides none (the loader gives every track one: a version being applied may pass through one). What readSendsValue() reads
// the song's sends from, and what the sends' part of the keeper follows the elements with (SendsArea.h), so that the two read
// the same tree the same way. A walk of the tracks' children, reading each SEND's ID alone.
struct HeldSendElement
{
    DeviceID id {};
    juce::ValueTree element {};
};

void forEachTrackHeldSends (
    const juce::ValueTree& song,
    const std::function<void (const juce::ValueTree& track, TrackID from, const std::vector<HeldSendElement>& sends)>&);

// A SEND element's ID as the song's sends are read by (forEachTrackHeldSends()): none for one the song holds no send by.
DeviceID readHeldSendID (const juce::ValueTree& sendElement);

// Every send of a song read from its tree (a PHAZITE element, or a whole copy of one): each TRACK's in the track list,
// a folder's before those of the tracks in it, each in its element's order, as the song holds them (readHeldSend()), one of
// each ID (of two, the first stays), none of a track without an ID (forEachTrackHeldSends()). For a pass over the song's
// sends.
SongSends readSendsValue (const juce::ValueTree& song);

// The song's sends whole, cheap to copy once the area holds them: read from its tree (readSendsValue()) while the tree holds
// them, the area's value once they leave it. For a pass over every send, not for one track's (Mixer.h: getSendValues()).
SongSends getSendsValue (const Song&);

// Whether the song holds a send with this ID (an invalid one: no), without a value made of its sends: what the selection asks
// of an automation point's owner (Selection.h). For a song whose tree holds its sends, a walk of each track's children.
bool holdsSend (const Song&, DeviceID);

// A send with a property given a value as the send would read it from an element that held it, or taken away (a void value:
// as if the element hadn't it): an ID and a return absent read as none, a level as 0, preFader as false; a level kept within
// its description's range. What a merge's clash on a send's property chosen sets, and what the conversion reads each
// property with. Whether the property is one of a send's (else nothing changes).
bool setSendProperty (SendValue&, const juce::Identifier& property, const juce::var& value);

//==============================================================================
// The IDs the sends hold and name (IDsHeld.h): the largest ID a send holds, below schema::maxID, 0 for none (the song's count
// of IDs stays past it); each send's own ID; and each return a send names (its sendTo), what goes with the return.
std::uint64_t findLargestIDIn (const SongSends&);
void forEachIDHeld (const SongSends&, const std::function<void (std::uint64_t)>&);
void forEachIDNamed (const SongSends&, const std::function<void (std::uint64_t)>&);
} // namespace phazite
