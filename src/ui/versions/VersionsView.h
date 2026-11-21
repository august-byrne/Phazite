// SPDX-License-Identifier: AGPL-3.0-or-later
// The project's versions, in the left panel (docs/design/versions.md: The Versions view), laid out as VS Code's Source Control
// view, in plain words:
// - at the top, its menu of what else it does, and the branch the song is on, whose button opens the branch picker
//   (version.pickBranch);
// - the version's name, whose first line is its name and the lines after it its note, both optional (Ctrl+Enter saves it),
//   then Save Version across the view, whose menu's part holds Bounce Each Version;
// (in VS Code's sizes: the title as its side bar's, the field and Save Version in rows as its own, the groups as rows of a
// tree, the history under a pane's header, the lists as wide as the panel, their rows inset)
// - Changes: what changed since the last version, a row for each track (the master, the song's own), each with its icon,
//   what changed in it, and its letter at its end, A added, M modified, D deleted (its name struck through); under the
//   pointer, its Discard Changes; on the heading, Discard All Changes. A click selects its track. Or, while a merge's list
//   shows, what the other branch changed, which Merge takes whole; while a merge waits for its version, its conflicts (C),
//   each switched between its values;
// - History: every branch's versions newest first, each on its branch's line in its color, a branch's name on its last
//   version, with a play mark on those with a bounce, which hears it in place of the song (the changes' row, the song again).
//   A click on a version opens it, as a tree's row opens: what it changed shows under it, a line a track, each with its letter
//   and, under the pointer, its Restore from This Version; another click folds it; a double-click opens it in the song's
//   place, live (Open Version), its row marked "Open", with a bar across the view's top that says so, with Back to Now and
//   Create Branch Here (Escape goes back too);
//   Between the versions, the automatic saves (the song as it was while it differed from its last version, kept without
//   asking: the model's snapshots), as Figma's and Google Docs' histories list theirs: in grey, by when each was taken
//   (above the versions saved before it, whichever branch they are on), those between two versions folded into one row
//   ("6 automatic saves") that a click unfolds, each with its Restore under the pointer; Show Automatic Saves
//   (version.showAutomatic, in the view's menu) hides them all;
// - Undo History, the song's undo steps, a click going back or forth to one, folded at first;
// - and the room the history takes.
// The sections fold under their headings. By the model's functions (model/versions/VersionsModel.h) and the commands.
#pragma once

#include "model/commands/UndoHistory.h"
#include "model/project/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "services/Settings.h"
#include "ui/components/ListRowHover.h"
#include "ui/components/SectionHeader.h"
#include "ui/components/SightWatcher.h"
#include "ui/components/ViewTitle.h"
#include "ui/input/CommandBridge.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>
#include <set>
#include <vector>

namespace phazite::ui
{
class VersionsView : public juce::Component,
                     private ProjectContext::Listener,
                     private juce::TextEditor::Listener
{
public:
    VersionsView (CommandBridge&, ProjectContext&, Settings&);
    ~VersionsView() override;

    void refreshTooltips(); // with the commands' keys, after the keymap changed

    void paint (juce::Graphics&) override;
    void resized() override;

    // What the list of changes shows: the changes (or what differs from a version compared with), a merge's lines, or a
    // merge's conflicts.
    enum class Lines
    {
        changes,
        merge,
        clashes
    };

    // For tests: what the lists show, a row at a time, as screen readers say it; a row's letter (A, M, D or C); and the parts.
    Lines getLines() const noexcept { return lines; }
    int getNumChanges() const noexcept;
    juce::String describeChange (int row) const;
    juce::String getChangeLetter (int row) const;
    int getNumHistoryRows() const noexcept { return (int) historyRows.size(); } // the changes' row first
    juce::String describeHistoryRow (int row) const;

    // The history's version unfolded, whose changes show under it (none for none), unfolded or folded as a click on it does; and
    // the row of a version, or of the changes (none).
    const juce::String& getUnfoldedVersion() const noexcept { return openedVersion; }
    void unfoldVersion (const juce::String& versionID);
    int getHistoryRow (const juce::String& versionID) const;
    juce::TextEditor& getMessageField() noexcept;

    juce::Button& getSaveButton() noexcept { return saveButton; }
    juce::Button& getSaveMenuButton() noexcept { return saveMenuButton; }
    juce::Button& getDoneButton() noexcept { return doneButton; }
    juce::Button& getMergeButton() noexcept { return mergeButton; }
    juce::Button& getCancelButton() noexcept { return cancelButton; }
    juce::Button& getBranchButton() noexcept;
    juce::Button& getMoreButton() noexcept { return moreButton; }
    juce::Button& getChangesSection() noexcept;

    juce::Button& getHistorySection() noexcept;
    juce::ListBox& getChangesList() noexcept { return changesList; }

    juce::ListBox& getHistoryList() noexcept { return historyList; }

    // The automatic saves, newest first, in the history between the versions: how many there are, one as screen readers say
    // it ("Automatic save, Before Restore, 5 min, on main"), its row in the history (none while its group is folded, or while
    // Show Automatic Saves is off), the row of the group it is folded into (none for one that stands alone between two
    // versions), a group's row unfolded or folded as a click on it does, and where a row's Restore is, under the pointer or
    // selected.
    int getNumSnapshots() const noexcept { return (int) snapshots.size(); }
    juce::String describeSnapshot (int snapshot) const;
    int getSnapshotRow (int snapshot) const;
    int getSnapshotGroupRow (int snapshot) const;
    void unfoldAutomaticSaves (int row);
    juce::Rectangle<int> getSnapshotRestorePosition (int row) const;
    void showAutomaticSavesChanged(); // the setting switched (version.showAutomatic)
    juce::String getChangesHeading() const;
    juce::String getRoomText() const { return roomText.getText(); }

    // What the view says while the branch has versions the song didn't grow from (none while it hasn't), and its button.
    juce::String getNewerText() const { return newerText.isVisible() ? newerText.getText() : juce::String(); }
    juce::Button& getNewerButton() noexcept { return newerButton; }

    // The bar across its top while a version is open in the song's place ("Looking at "Verse 2" (3 days ago)"; none while none
    // is), with Back to Now, Create Branch Here, and, where the version's plug-ins aren't installed here and it has a bounce,
    // Hear Its Bounce. Escape, while the view has the keys, goes back to Now.
    juce::String getOpenBarText() const { return openText.isVisible() ? openText.getText() : juce::String(); }
    juce::Button& getBackToNowButton() noexcept { return backButton; }
    juce::Button& getBranchHereButton() noexcept { return branchHereButton; }
    juce::Button& getOpenBounceButton() noexcept { return openBounceButton; }
    bool keyPressed (const juce::KeyPress&) override;

    // Undo History, at the foot (folded at first; as Ableton Live 12.1's, Logic Pro's and Studio One's): the
    // song's undo steps newest first, those undone (which Redo would do again) dimmed above the one the song is at, and under
    // the oldest, the song before them all. A click on a row, or Return on the one selected, goes back or forth to it, undoing
    // or redoing as Undo and Redo do (model/commands/UndoHistory.h). Its section, its list, its rows (none while it has no
    // steps), a row as screen readers say it, and the row the song is at (none without steps).
    juce::Button& getUndoHistorySection() noexcept;
    juce::ListBox& getUndoHistoryList() noexcept { return undoList; }
    int getNumUndoRows() const noexcept;
    juce::String describeUndoRow (int row) const;
    int getUndoNowRow() const noexcept;

    // The menus of Save Version's part and of the view's three dots, as their buttons show them.
    juce::PopupMenu makeSaveMenu();
    juce::PopupMenu makeMoreMenu();

    // Where things are that show only under the pointer (or on the selected row): a history row's play mark, while any
    // version has a bounce (and the row has one); and a change's Discard Changes, in the changes list. Empty while they don't
    // show.
    juce::Rectangle<int> getPlayMarkPosition (int row) const;
    juce::Rectangle<int> getPutBackPosition (int row) const;
    juce::Rectangle<int> getRestorePosition (int row) const; // a line under an opened version, in the history

    // The menu of the history's row (a version's, the changes', or a line under a version), as a right-click shows it.
    juce::PopupMenu makeHistoryMenu (int row);

    // When a version was saved, as the history shows it: "now", "5 min", "2 h", the day this week ("Mon"), or the date.
    static juce::String describeWhen (juce::Time saved, juce::Time now);

private:
    class ChangesModel;
    class HistoryModel;
    class UndoHistoryModel;
    class MessageField;
    class BranchButton;

    CommandBridge& commands;
    ProjectContext& context;
    Settings& settings;

    ViewTitle title { "Versions" }; // with the view's menu
    juce::Label emptyText, roomText;
    std::unique_ptr<BranchButton> branchButton;
    IconButton moreButton { "More Actions", Icon::moreActions };
    std::unique_ptr<MessageField> messageField;
    IconButton saveButton { "Save Version", Icon::check }, saveMenuButton { "Save Version's choices", Icon::expanded };
    std::unique_ptr<SectionHeader> changesSection, historySection, undoSection;
    juce::TextButton doneButton;                // from another version's differences back to the changes
    juce::TextButton mergeButton, cancelButton; // a merge's list's

    // While the branch has versions the song didn't grow from (another computer's, or newer than a project file put back):
    // what the view says of them, under Save Version, and Take (or Merge) the Newer Versions.
    juce::Label newerText;
    juce::TextButton newerButton;

    // The version open in the song's place (none while none is), and the bar that says so (getOpenBarText()).
    juce::String shownVersion;
    juce::Label openText;
    juce::TextButton backButton, branchHereButton, openBounceButton;
    void refreshOpenBar();
    std::unique_ptr<ChangesModel> changesModel;
    std::unique_ptr<HistoryModel> historyModel;
    std::unique_ptr<UndoHistoryModel> undoModel;
    juce::ListBox changesList, historyList, undoList;
    std::unique_ptr<ListRowHover> changesHover, historyHover, undoHover;
    std::vector<SnapshotInfo> snapshots;

    // The song's undo history as the list last showed it, read again as the song changes; and going to a row's step.
    UndoHistoryInfo undoHistory;
    void refreshUndoHistory();
    void goToUndoRow (int row);

    Lines lines = Lines::changes;
    std::vector<VersionChange> changes;
    std::vector<MergeLine> mergeLines;
    std::vector<ClashInfo> clashes;
    std::vector<VersionInfo> versions;
    std::vector<BranchInfo> branches;
    juce::String currentBranch;
    juce::String heardVersion;    // none while the song itself is heard
    bool anyBounce = false;       // the history shows play marks
    juce::String bouncingVersion; // an old version's, rendering in the background, and how far it has come
    float bounceProgress = 0.0f;
    bool hasPlayMark (int row) const;

    // The history's rows: the changes, then each version, and under the one opened, a row for each line it changed; and
    // before each version, the automatic saves taken since it: one alone as its own row, more as a group's row, with a row
    // for each under it while the group is unfolded.
    struct HistoryRow
    {
        int version = -1;  // its version's place in versions; none for the changes' row and the automatic saves'
        int change = -1;   // its line's place in openedChanges; none for the version itself
        int snapshot = -1; // an automatic save's place in snapshots
        int group = -1;    // a group's place in snapshotGroups: its own row (no snapshot), or the row of one in it
    };

    // The automatic saves between two versions, folded into one row: its first, how many, and the version they were taken
    // since (none for those older than every version), which names the group while it is unfolded or selected.
    struct SnapshotGroup
    {
        int first = 0, count = 0;
        juce::String since;
    };

    std::vector<HistoryRow> historyRows;
    std::vector<SnapshotGroup> snapshotGroups;
    std::set<juce::String> unfoldedGroups;              // by SnapshotGroup::since
    std::optional<juce::String> selectedGroup;          // the group whose row is selected, by the same
    juce::String changesFollow;                         // the version the changes' row's line goes to
    const SnapshotInfo* getRowSnapshot (int row) const; // an automatic save's row's
    const SnapshotGroup* getRowGroup (int row) const;   // a group's own row's
    juce::String openedVersion;
    std::vector<VersionChange> openedChanges;
    const VersionInfo* getRowVersion (int row) const;  // a version's row's version (not a line's under it)
    const VersionChange* getRowChange (int row) const; // a line's under its version
    const VersionInfo* getLineVersion (int row) const; // the version a line under it is of
    void refreshHistoryRows();
    void selectHistoryRow(); // the row of what the model selects, after the rows moved

    // The changes' icons (a track's, the master's, the song's) and their action's, drawn in each row's colors; and the
    // history's chevron on each version, open or folded.
    TintedIcon trackIcon { Icon::sound }, masterIcon { Icon::effect }, songIcon { Icon::song };
    TintedIcon discardIcon { Icon::undo };
    TintedIcon openIcon { Icon::expanded }, foldedIcon { Icon::folded };
    TintedIcon clockIcon { Icon::recent }; // an automatic save's
    TintedIcon redoIcon { Icon::redo };    // an undone step of Undo History's, which Redo would do again

    // The history's lines, from a row to the row of a version it follows: down the column of the newer row's branch, turning
    // into the older's at its end (a version's first parent: its own branch's line, or where its branch started), or turning
    // at the newer end and down the older's column (a merge's other parent).
    struct HistoryLine
    {
        int top = 0, bottom = 0;         // the rows, the newer first
        int topLane = 0, bottomLane = 0; // their branches' columns
        bool turnsAtBottom = true;
        juce::Colour colour;
    };

    std::vector<HistoryLine> historyLines;

    void refresh(); // what the model holds now
    void refreshHistoryLines();

    // While it can't be seen (another view in the left panel, the panel hidden), it reads and makes nothing: it is always
    // there, usually hidden, and the versions change a few times a second while the song is edited, each change read whole
    // (every version, every automatic save, the changes, the room) with all its rows. It notes that it is out of date, and
    // reads the model as it shows (isInSight(), SightWatcher.h). What else tells of the versions reads the model itself: the
    // activity bar's badge (countChanges()), the commands' states.
    bool stale = false;
    SightWatcher sight { *this, [this] { refreshWhereStale(); } };
    void refreshInSight();    // refresh() if it can be seen, else noted
    void refreshWhereStale(); // what changed while it couldn't be seen, once it can

    // Bounce Each Version, as the settings keep it, for the song open now: kept while the view is hidden, as it is the model's.
    void followBounceSetting();

    // The field as typed, the model's draft: its first line the name, the rest the note. And the draft as one text, and as it
    // was when the view last took or showed it, so that the view shows the draft again only when it changed in the model.
    void takeMessage();
    juce::String getDraftMessage() const;
    juce::String takenMessage;

    void showMenu (std::initializer_list<const char*> commandIDs); // for the row under the pointer, which is selected
    void showClashMenu (int clash);
    bool showsActions (int row) const;                // a change's, under the pointer, or selected
    int getLane (const juce::String& branchID) const; // the branch's line's column in the history
    juce::Colour getBranchColour (const juce::String& branchID) const;
    juce::String getBranchName (const juce::String& branchID) const;

    void projectChanged() override
    {
        unfoldedGroups.clear(); // another project's
        selectedGroup.reset();
        followBounceSetting();
        refreshInSight();
    }
    void versionsChanged() override
    {
        followBounceSetting();
        refreshInSight();
    }
    void songChanged (const SongChanges& changed) override
    {
        if (! isInSight (*this))
        {
            stale = true;
            return;
        }

        refreshUndoHistory(); // each change is a step, or a step's part

        if (changed.devices && shownVersion.isNotEmpty())
            refreshOpenBar(); // a plug-in the version has, found missing as it joins
    }
    void textEditorTextChanged (juce::TextEditor&) override;
};
} // namespace phazite::ui
