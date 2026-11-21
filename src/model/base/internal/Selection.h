// SPDX-License-Identifier: AGPL-3.0-or-later
// What is selected in the open project (roadmap A5, 2026-10-30): tracks, clips, sections and devices by their IDs, in the
// order they were selected; or notes of the clips an editor shows, by their references (a clip's ID and the note's); or
// automation points of one parameter, by their elements in the song, which a project saves without IDs. One kind of thing
// at a time, as the commands act on one: selecting devices deselects anything else and the other way round, and notes and
// points are each all that is selected while they are. What is gone (an item, a note, a point) is left out. The model's commands act on what it names in its song (the project context's), and the UI reaches it by IDs
// (model/arrangement/SongSelection.h). It tells its listeners after each change, on the message thread. Message thread
// only.
#pragma once

#include "model/base/Song.h"
#include "model/devices/ParameterRef.h"

#include <vector>

namespace phazite
{
class Selection : public juce::ChangeBroadcaster
{
public:
    Selection() = default;

    // The song whose items it names (model/base/Song.h), or none: another forgets what was selected.
    void setSong (Song*);
    Song* getSong() const noexcept { return heldSong.get(); } // none once it is deleted

    //==============================================================================
    // Tracks (folders, returns and the master too), clips and devices (plug-ins, containers), by their IDs, in the
    // order they were selected: what the model's commands act on. An item the song no longer holds is left out.
    enum class Kind
    {
        track,
        clip,
        device
    };

    bool isSelected (TrackID id) const { return isSelected (Item { Kind::track, id.getRaw() }); }
    void selectOnly (TrackID id) { selectOnly (Item { Kind::track, id.getRaw() }); }
    void addToSelection (TrackID id) { addToSelection (Item { Kind::track, id.getRaw() }); }
    void deselect (TrackID id) { deselect (Item { Kind::track, id.getRaw() }); }
    bool isSelected (ClipID id) const { return isSelected (Item { Kind::clip, id.getRaw() }); }
    void selectOnly (ClipID id) { selectOnly (Item { Kind::clip, id.getRaw() }); }
    void addToSelection (ClipID id) { addToSelection (Item { Kind::clip, id.getRaw() }); }
    void deselect (ClipID id) { deselect (Item { Kind::clip, id.getRaw() }); }
    bool isSelected (DeviceID id) const { return isSelected (Item { Kind::device, id.getRaw() }); }
    void selectOnly (DeviceID id) { selectOnly (Item { Kind::device, id.getRaw() }); }
    void addToSelection (DeviceID id) { addToSelection (Item { Kind::device, id.getRaw() }); }
    void deselect (DeviceID id) { deselect (Item { Kind::device, id.getRaw() }); }

    std::vector<TrackID> getTracks() const;
    std::vector<ClipID> getClips() const;
    std::vector<DeviceID> getDevices() const;

    void deselectAll();
    int getNumObjectsSelected() const; // of every kind still there, notes and points included

    //==============================================================================
    // Sections, which the project keeps itself (model/arrangement/internal/Sections.h), by their IDs: with tracks and
    // clips, and without devices, as tracks and clips are.
    bool isSelected (SectionID) const;
    void selectOnly (SectionID);
    void addToSelection (SectionID);
    void deselect (SectionID);

    //==============================================================================
    // Notes, of the clips an editor shows (the piano roll's), by their references: other clips deselect them, and so does
    // selecting anything else.
    void setNoteClips (const std::vector<ClipID>&);
    const std::vector<ClipID>& getNoteClips() const noexcept { return noteClips; }
    const std::vector<NoteRef>& getNotes() const noexcept { return notes; }      // as selected: some may be gone
    void selectNotes (const std::vector<NoteRef>&, bool addToSelection = false); // of the note clips

    //==============================================================================
    // Automation points of one parameter, by their elements in the song (Schema.h: POINT), which stay the same while points
    // are added or removed around them, moved, or undone and redone around them, so that the selection follows them where
    // indices would go stale; the elements' order and which are still there are the curve's to say
    // (automation/internal/Automation.h: getSelectedPoints()). Selecting anything else deselects them.
    const ParameterRef& getPointsParameter() const noexcept { return pointsParameter; }       // invalid while none are selected
    const std::vector<juce::ValueTree>& getPoints() const noexcept { return points; }         // as selected: some may be gone
    void selectPoints (const ParameterRef&, const std::vector<juce::ValueTree>& pointStates); // each once, by their beats

    // How many times points have been compared with each other or looked for by their beats, here and by
    // getSelectedPoints(), since Phazite started: for the test that this grows with the points selected, not with their
    // square. Message thread only.
    static std::uint64_t getPointComparisons() noexcept;
    static void countPointComparisons (std::uint64_t) noexcept;

    // The same for notes: how many times a note was compared with the notes chosen as notes are selected.
    static std::uint64_t getNoteComparisons() noexcept;

private:
    // A track, a clip or a device by its kind and its ID.
    struct Item
    {
        Kind kind = Kind::track;
        std::uint64_t id = 0;

        bool operator== (const Item&) const = default;
    };

public:
    // What is selected, every kind of it, as it is (what is gone included, which may come back), to be put back so later
    // whatever was selected meanwhile: a version opened in the song's place, and Back to Now (model/versions/VersionsModel.h).
    struct Kept
    {
        std::vector<Item> items;
        std::vector<SectionID> sections;
        std::vector<ClipID> noteClips;
        std::vector<NoteRef> notes;
        ParameterRef pointsParameter;
        std::vector<juce::ValueTree> points;
    };

    Kept keep() const;
    void putBack (const Kept&);

private:
    juce::WeakReference<Song> heldSong;
    std::vector<Item> items;
    std::vector<SectionID> sections;
    std::vector<ClipID> noteClips;
    std::vector<NoteRef> notes;
    ParameterRef pointsParameter;
    std::vector<juce::ValueTree> points;

    bool isThere (Item) const; // whether the song holds it
    bool isThere (SectionID) const;
    bool isSelected (Item) const;
    void selectOnly (Item);
    void addToSelection (Item);
    void deselect (Item);
    bool holdsDevices() const;
    void clearItems();          // tracks, clips, devices and sections, without telling
    bool clearNotesAndPoints(); // without telling; whether there were any
    void changed() { sendChangeMessage(); }

    template <typename ID>
    std::vector<ID> getIDs (Kind kind) const
    {
        std::vector<ID> ids;

        for (const auto& item : items)
            if (item.kind == kind && isThere (item))
                ids.push_back (ID::fromRaw (item.id));

        return ids;
    }

    JUCE_DECLARE_NON_COPYABLE (Selection)
};

// Whether the song holds a note: its clip is the song's, and holds a note of its ID. Here, below the notes' code
// (model/notes/internal/Notes.h, which includes this), for the selection, which counts the notes it names that are still
// there; and for the notes' code and the tests. Message thread.
bool hasNote (const Song&, const NoteRef&);
} // namespace phazite
