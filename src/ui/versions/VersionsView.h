// SPDX-License-Identifier: AGPL-3.0-or-later
// The project's versions, in the left panel (docs/versions.md: The Versions view): the branch, whose menu switches to another
// or makes one; a version saved with a name and a note, both optional; the changes since the current branch's last version,
// by track, a click on one selecting its track (or, while a merge's list shows, what the other line changed, ticked to take
// it; while a merge waits for its version, its clashes, each switched between its values); and the history, every branch's
// versions newest first, each on its branch's line in its color. By the model's functions (model/versions/VersionsModel.h)
// and the versions' commands.
#pragma once

#include "model/project/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "ui/input/CommandBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class VersionsView : public juce::Component,
                     private ProjectContext::Listener,
                     private juce::TextEditor::Listener
{
public:
    VersionsView (CommandBridge&, ProjectContext&);
    ~VersionsView() override;

    // The keyboard to the version's name, as showing the view gives it the keyboard.
    void focusName();
    void refreshTooltips(); // with the commands' keys, after the keymap changed

    void paint (juce::Graphics&) override;
    void resized() override;

    // What its list of lines shows: the changes (since the last version, or from a version taken from), a merge's lines, or a
    // merge's clashes.
    enum class Lines
    {
        changes,
        merge,
        clashes
    };

    // For tests: what the lists show, a row at a time, as screen readers say it.
    Lines getLines() const noexcept { return lines; }
    int getNumChanges() const noexcept;
    juce::String describeChange (int row) const;
    int getNumHistoryRows() const noexcept { return (int) versions.size() + 1; } // "Now" first
    juce::String describeHistoryRow (int row) const;
    juce::TextEditor& getNameField() noexcept { return nameField; }
    juce::TextEditor& getNoteField() noexcept { return noteField; }
    juce::Button& getSaveButton() noexcept { return saveButton; }
    juce::Button& getDoneButton() noexcept { return doneButton; }
    juce::Button& getMergeButton() noexcept { return mergeButton; }
    juce::Button& getCancelButton() noexcept { return cancelButton; }
    juce::Button& getBranchButton() noexcept { return branchButton; }
    juce::ListBox& getChangesList() noexcept { return changesList; }
    juce::String getChangesHeading() const { return changesHeading.getText(); }

    // When a version was saved, as the history shows it: "now", "5 min", "2 h", the day this week ("Mon"), or the date.
    static juce::String describeWhen (juce::Time saved, juce::Time now);

private:
    class ChangesModel;
    class HistoryModel;

    CommandBridge& commands;
    ProjectContext& context;

    juce::Label title, changesHeading, historyHeading, emptyText;
    juce::TextButton branchButton;
    juce::TextEditor nameField, noteField;
    juce::TextButton saveButton;
    juce::TextButton doneButton;                // from another version's differences back to the changes
    juce::TextButton mergeButton, cancelButton; // a merge's list's
    std::unique_ptr<ChangesModel> changesModel;
    std::unique_ptr<HistoryModel> historyModel;
    juce::ListBox changesList, historyList;

    Lines lines = Lines::changes;
    std::vector<VersionChange> changes;
    std::vector<MergeLine> mergeLines;
    std::vector<ClashInfo> clashes;
    std::vector<VersionInfo> versions;
    std::vector<BranchInfo> branches;
    juce::String currentBranch;

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
    void showMenu (std::initializer_list<const char*> commandIDs); // for the row under the pointer, which is selected
    void showBranchMenu();
    void showClashMenu (int clash);
    int getLane (const juce::String& branchID) const; // the branch's line's column in the history
    juce::Colour getBranchColour (const juce::String& branchID) const;
    juce::String getBranchName (const juce::String& branchID) const;

    void projectChanged() override { refresh(); }
    void versionsChanged() override { refresh(); }
    void textEditorTextChanged (juce::TextEditor&) override;
    void textEditorReturnKeyPressed (juce::TextEditor&) override;
};
} // namespace phazite::ui
