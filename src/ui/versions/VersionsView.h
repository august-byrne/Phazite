// SPDX-License-Identifier: AGPL-3.0-or-later
// The project's source control, in the left panel (docs/versions.md: The Versions view), laid out as VS Code's Source
// Control view and named in git's words:
// - at the top, its menu of what else it does, and the branch the song is on, whose button opens the branch picker
//   (version.pickBranch);
// - the commit's message, whose first line is its name and the lines after it its note, both optional (Ctrl+Enter commits),
//   then Commit across the view, whose menu's part holds Bounce Each Commit;
// (in VS Code's sizes: the title as its side bar's, the message and Commit in rows as its own, the groups as rows of a tree, the
// Graph under a pane's header, the lists as wide as the panel, their rows inset)
// - Staged Changes, while any is staged: what the next commit records, each line's Unstage under the pointer;
// - Changes: what changed in the working tree, a row for each track (the master, the song's own), each with its icon, what
//   changed in it, and its letter at its end as git marks it, U untracked, A added, M modified, D deleted (its name struck
//   through), in VS Code's colors; under the pointer, its Discard Changes and Stage Changes; on the heading, Discard All and
//   Stage All. A click selects its track. Or, while a merge's list shows, what the other line changed, ticked to take it;
//   while a merge waits for its commit, its conflicts, each switched between its values;
// - Graph: every branch's commits newest first, each on its branch's line in its color, a branch's name on its last commit,
//   with a play mark on those with a bounce, which hears it in place of the song (the uncommitted changes', the song again).
//   A click on a commit opens it, as a tree's row opens: what it changed shows under it, a line a track, each with its
//   letter and, under the pointer, its Restore from This Commit; another click folds it;
// - and the room the history takes.
// The sections fold under their headings. By the model's functions (model/versions/VersionsModel.h) and the commands.
#pragma once

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

    // What the list of changes shows: the working tree's (or what differs from a commit compared with), a merge's lines, or a
    // merge's conflicts.
    enum class Lines
    {
        changes,
        merge,
        clashes
    };

    // For tests: what the lists show, a row at a time, as screen readers say it; a row's letter (U, A, M or D); and the parts.
    Lines getLines() const noexcept { return lines; }
    int getNumChanges() const noexcept;
    juce::String describeChange (int row) const;
    juce::String getChangeLetter (int row) const;
    int getNumStaged() const noexcept { return (int) staged.size(); }
    juce::String describeStaged (int row) const;
    juce::String getStagedLetter (int row) const;
    int getNumHistoryRows() const noexcept { return (int) historyRows.size(); } // the uncommitted changes first
    juce::String describeHistoryRow (int row) const;

    // The graph's commit opened, whose changes show under it (none for none), opened or folded as a click on it does; and the
    // row of a commit, or of the uncommitted changes (none).
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
    juce::Button& getStagedSection() noexcept;
    juce::Button& getHistorySection() noexcept;
    juce::ListBox& getChangesList() noexcept { return changesList; }
    juce::ListBox& getStagedList() noexcept { return stagedList; }
    juce::ListBox& getHistoryList() noexcept { return historyList; }

    // Local History, under the graph (folded at first): its section, its list, a row of it as screen readers say it, and
    // where a row's Restore is, under the pointer or selected.
    juce::Button& getLocalHistorySection() noexcept;
    juce::ListBox& getLocalHistoryList() noexcept { return localList; }
    int getNumSnapshots() const noexcept { return (int) snapshots.size(); }
    juce::String describeSnapshot (int row) const;
    juce::Rectangle<int> getSnapshotRestorePosition (int row) const;
    juce::String getChangesHeading() const;
    juce::String getRoomText() const { return roomText.getText(); }

    // The menus of Commit's part and of the view's three dots, as their buttons show them.
    juce::PopupMenu makeSaveMenu();
    juce::PopupMenu makeMoreMenu();

    // Where things are that show only under the pointer (or on the selected row): a graph row's play mark, while any commit
    // has a bounce (and the row has one); a change's Discard Changes and Stage Changes, in the changes list; and a staged
    // line's Unstage Changes, in the staged list. Empty while they don't show.
    juce::Rectangle<int> getPlayMarkPosition (int row) const;
    juce::Rectangle<int> getPutBackPosition (int row) const;
    juce::Rectangle<int> getStagePosition (int row) const;
    juce::Rectangle<int> getUnstagePosition (int row) const;
    juce::Rectangle<int> getRestorePosition (int row) const; // a line under an opened commit, in the graph

    // The menu of the graph's row (a commit's, the uncommitted changes', or a line under a commit), as a right-click shows it.
    juce::PopupMenu makeHistoryMenu (int row);

    // When a commit was made, as the graph shows it: "now", "5 min", "2 h", the day this week ("Mon"), or the date.
    static juce::String describeWhen (juce::Time saved, juce::Time now);

private:
    class ChangesModel;
    class HistoryModel;
    class LocalHistoryModel;
    class MessageField;
    class BranchButton;

    CommandBridge& commands;
    ProjectContext& context;
    Settings& settings;

    ViewTitle title { "Source Control" }; // with the view's menu
    juce::Label emptyText, roomText;
    std::unique_ptr<BranchButton> branchButton;
    IconButton moreButton { "More Actions", Icon::moreActions };
    std::unique_ptr<MessageField> messageField;
    IconButton saveButton { "Commit", Icon::check }, saveMenuButton { "Commit's choices", Icon::expanded };
    std::unique_ptr<SectionHeader> stagedSection, changesSection, historySection, localSection;
    juce::TextButton doneButton;                // from another commit's differences back to the changes
    juce::TextButton mergeButton, cancelButton; // a merge's list's
    std::unique_ptr<ChangesModel> stagedModel, changesModel;
    std::unique_ptr<HistoryModel> historyModel;
    std::unique_ptr<LocalHistoryModel> localModel;
    juce::ListBox stagedList, changesList, historyList, localList;
    std::unique_ptr<ListRowHover> stagedHover, changesHover, historyHover, localHover;
    std::vector<SnapshotInfo> snapshots;

    Lines lines = Lines::changes;
    std::vector<VersionChange> staged, changes;
    std::vector<MergeLine> mergeLines;
    std::vector<ClashInfo> clashes;
    std::vector<VersionInfo> versions;
    std::vector<BranchInfo> branches;
    juce::String currentBranch;
    juce::String heardVersion;    // none while the song itself is heard
    bool anyBounce = false;       // the graph shows play marks
    juce::String bouncingVersion; // an old commit's, rendering in the background, and how far it has come
    float bounceProgress = 0.0f;
    bool hasPlayMark (int row) const;

    // The graph's rows: the uncommitted changes, then each commit, and under the one opened, a row for each line it changed.
    struct HistoryRow
    {
        int version = -1; // its commit's place in versions; none for the uncommitted changes
        int change = -1;  // its line's place in openedChanges; none for the commit itself
    };

    std::vector<HistoryRow> historyRows;
    juce::String openedVersion;
    std::vector<VersionChange> openedChanges;
    const VersionInfo* getRowVersion (int row) const;  // a commit's row's commit (not a line's under it)
    const VersionChange* getRowChange (int row) const; // a line's under its commit
    const VersionInfo* getLineVersion (int row) const; // the commit a line under it is of
    void refreshHistoryRows();
    void selectHistoryRow(); // the row of what the model selects, after the rows moved

    // The changes' icons (a track's, the master's, the song's) and their actions', drawn in each row's colors; and the
    // graph's chevron on each commit, open or folded.
    TintedIcon trackIcon { Icon::sound }, masterIcon { Icon::effect }, songIcon { Icon::song };
    TintedIcon discardIcon { Icon::undo }, stageIcon { Icon::plus }, unstageIcon { Icon::minus };
    TintedIcon openIcon { Icon::expanded }, foldedIcon { Icon::folded };
    TintedIcon clockIcon { Icon::recent }; // a snapshot of Local History's

    // The graph's lines, from a row to the row of a commit it follows: down the column of the newer row's branch, turning into
    // the older's at its end (a commit's first parent: its own branch's line, or where its branch started), or turning at the
    // newer end and down the older's column (a merge's other parent).
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

    // The message as typed, the model's draft: its first line the name, the rest the note. And the draft as one message, and
    // as it was when the view last took or showed it, so that the view shows the draft again only when it changed in the model.
    void takeMessage();
    juce::String getDraftMessage() const;
    juce::String takenMessage;

    void showMenu (std::initializer_list<const char*> commandIDs); // for the row under the pointer, which is selected
    void showClashMenu (int clash);
    bool showsActions (const juce::ListBox&, int row) const; // under the pointer, or selected
    juce::Rectangle<int> getActionPosition (const juce::ListBox&, int row, int fromRight) const;
    int getLane (const juce::String& branchID) const; // the branch's line's column in the graph
    juce::Colour getBranchColour (const juce::String& branchID) const;
    juce::String getBranchName (const juce::String& branchID) const;

    void projectChanged() override { refresh(); }
    void versionsChanged() override { refresh(); }
    void textEditorTextChanged (juce::TextEditor&) override;
};
} // namespace phazite::ui
