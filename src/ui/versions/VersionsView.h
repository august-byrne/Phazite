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
//   and, under the pointer, its Restore from This Version; another click folds it; a double-click hears it (Hear It);
// - Local History, the song as it was while it differed from its last version, and Undo History, the song's undo steps, a
//   click going back or forth to one (docs/design.md, open question 22), each folded at first;
// - and the room the history takes.
// The sections fold under their headings. By the model's functions (model/versions/VersionsModel.h) and the commands.
#pragma once

#include "model/commands/UndoHistory.h"
#include "model/project/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "services/Settings.h"
#include "ui/components/ListRowHover.h"
#include "ui/components/SectionHeader.h"
#include "ui/components/ViewTitle.h"
#include "ui/input/CommandBridge.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

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

    // The history's version opened, whose changes show under it (none for none), opened or folded as a click on it does; and
    // the row of a version, or of the changes (none).
    const juce::String& getOpenedVersion() const noexcept { return openedVersion; }
    void openVersion (const juce::String& versionID);
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

    // Local History, under the history (folded at first): its section, its list, a row of it as screen readers say it, and
    // where a row's Restore is, under the pointer or selected.
    juce::Button& getLocalHistorySection() noexcept;
    juce::ListBox& getLocalHistoryList() noexcept { return localList; }
    int getNumSnapshots() const noexcept { return (int) snapshots.size(); }
    juce::String describeSnapshot (int row) const;
    juce::Rectangle<int> getSnapshotRestorePosition (int row) const;
    juce::String getChangesHeading() const;
    juce::String getRoomText() const { return roomText.getText(); }

    // Undo History, at the foot (folded at first, as Local History; as Ableton Live 12.1's, Logic Pro's and Studio One's): the
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
    class LocalHistoryModel;
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
    std::unique_ptr<SectionHeader> changesSection, historySection, localSection, undoSection;
    juce::TextButton doneButton;                // from another version's differences back to the changes
    juce::TextButton mergeButton, cancelButton; // a merge's list's
    std::unique_ptr<ChangesModel> changesModel;
    std::unique_ptr<HistoryModel> historyModel;
    std::unique_ptr<LocalHistoryModel> localModel;
    std::unique_ptr<UndoHistoryModel> undoModel;
    juce::ListBox changesList, historyList, localList, undoList;
    std::unique_ptr<ListRowHover> changesHover, historyHover, localHover, undoHover;
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

    // The history's rows: the changes, then each version, and under the one opened, a row for each line it changed.
    struct HistoryRow
    {
        int version = -1; // its version's place in versions; none for the changes' row
        int change = -1;  // its line's place in openedChanges; none for the version itself
    };

    std::vector<HistoryRow> historyRows;
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
    TintedIcon clockIcon { Icon::recent }; // a snapshot of Local History's
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
    void refreshHistoryLines (const juce::String& nowFollows);

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

    void projectChanged() override { refresh(); }
    void versionsChanged() override { refresh(); }
    void songChanged (const SongChanges&) override { refreshUndoHistory(); } // each change is a step, or a step's part
    void textEditorTextChanged (juce::TextEditor&) override;
};
} // namespace phazite::ui
