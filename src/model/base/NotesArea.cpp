// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/NotesArea.h"
#include "Song.h"
#include "internal/IDsHeld.h"
#include "internal/WholeState.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // Two entries of the notes' area alike without a look inside: one clip's, the same value (ValueArea.h).
    struct SameNotes
    {
        bool operator() (const std::pair<std::uint64_t, ClipNotes>& a, const std::pair<std::uint64_t, ClipNotes>& b) const noexcept
        {
            return a.first == b.first && a.second.isSameAs (b.second);
        }
    };
} // namespace

//==============================================================================
SongValues::NotesTeller::NotesTeller (ValueArea<NotesByClip>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::NotesTeller::~NotesTeller()
{
    area.removeListener (this);
}

void SongValues::NotesTeller::valueAreaChanged (const NotesByClip& before, const NotesByClip& after)
{
    if (listeners.isEmpty())
        return;

    std::vector<ClipID> changed;
    const auto note = [&changed] (const std::pair<std::uint64_t, ClipNotes>& entry) { changed.push_back (ClipID::fromRaw (entry.first)); };

    before.clips.impl().template diff<SameNotes> (
        after.clips.impl(),
        immer::make_differ (note, note,
                            [&changed] (const std::pair<std::uint64_t, ClipNotes>&, const std::pair<std::uint64_t, ClipNotes>& entry)
                            { changed.push_back (ClipID::fromRaw (entry.first)); }));

    if (changed.empty())
        return;

    std::sort (changed.begin(), changed.end());
    listeners.call ([&changed] (SongValues::Listener& listener) { listener.clipNotesChanged (changed); });
}

//==============================================================================
const ClipNotes* findGroupNotes (const SongClips& clips, const NotesByClip& notes, std::uint64_t link, ClipID except)
{
    if (link == 0)
        return nullptr;

    if (const auto* members = clips.groups.find (link))
        for (const auto member : *members)
            if (member != except)
                if (const auto* held = notes.find (member))
                    return held;

    return nullptr;
}

void takeClipNotes (NotesByClip& notes, const SongClips& clips, const ClipValue& clip, const juce::ValueTree& element)
{
    const auto id = clip.id;

    if (clip.isMidi())
    {
        if (const auto* group = findGroupNotes (clips, notes, clip.link, id))
            notes.clips = notes.clips.set (id.getRaw(), *group);
        else
            notes.clips = notes.clips.set (id.getRaw(), readClipNotes (element));
    }
    else
    {
        notes.clips = notes.clips.erase (id.getRaw());
    }
}

//==============================================================================
void SongValues::setAll (Song& song, NotesByClip next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (song.isWritingOutsideHistory())
        notes.setOutsideHistory (std::move (next));
    else
        notes.set (std::move (next));
}

void SongValues::setWithoutStep (Song& song, NotesByClip next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (song.isWritingOutsideHistory())
        notes.setOutsideHistory (std::move (next));
    else
        notes.setWithoutStep (std::move (next));
}

void SongValues::setClipNotes (Song& song, ClipID clip, ClipNotes value)
{
    jassert (song.getValues() == this);
    const auto* clipValue = clips.get().find (clip);

    if (clipValue == nullptr || ! clipValue->isMidi())
    {
        jassertfalse; // a MIDI clip of the song's alone holds notes (the notes' code asks: Notes.h)
        return;
    }

    // It and its alias group's other MIDI clips, which hold one value.
    std::vector<ClipID> members;

    if (const auto* group = clipValue->link != 0 ? clips.get().groups.find (clipValue->link) : nullptr)
    {
        for (const auto member : *group)
            if (const auto* each = clips.get().find (member); each != nullptr && each->isMidi())
                members.push_back (member);
    }
    else
    {
        members.push_back (clip);
    }

    // The notes each holds already: no change of the song's. Held as one value, nothing; held as values alike, this one,
    // without a step of its own.
    const ClipNotes* first = nullptr;
    auto alike = true, one = true;

    for (const auto member : members)
    {
        const auto* held = notes.get().find (member);
        alike = alike && held != nullptr && *held == value;

        if (! alike)
            break;

        if (first == nullptr)
            first = held;
        else
            one = one && held->isSameAs (*first);
    }

    if (alike && one)
        return;

    auto next = notes.get();

    for (const auto member : members)
        next.clips = next.clips.set (member.getRaw(), value);

    if (alike)
        setWithoutStep (song, std::move (next));
    else
        setAll (song, std::move (next));
}

//==============================================================================
bool holdSameNotes (const ValuesKept& a, const ValuesKept& b, ClipID clip)
{
    const auto* first = a.notes.find (clip);
    const auto* second = b.notes.find (clip);

    if (first == nullptr || second == nullptr)
        return first == second;

    return *first == *second;
}

std::uint64_t findLargestIDIn (const NotesByClip& notes)
{
    std::uint64_t largest = 0;

    for (const auto& entry : notes.clips)
        for (const auto& note : entry.second.notes)
            largest = countHeldID (largest, note.id.getRaw());

    return largest;
}
} // namespace phazite
