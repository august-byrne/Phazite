// SPDX-License-Identifier: AGPL-3.0-or-later
// The project's versions, in the left panel (docs/versions.md: The Versions view): a version saved with a name and a note,
// both optional; the changes since the current branch's last version, by track, a click on one selecting its track; and
// the history, every branch's versions newest first, each on its branch's line in its color. By the model's functions
// (model/versions/VersionsModel.h); Save runs version.save, which saves the project first.
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

    // For tests: what the lists show, a row at a time, as screen readers say it.
    int getNumChanges() const noexcept { return (int) changes.size(); }
    juce::String describeChange (int row) const;
    int getNumHistoryRows() const noexcept { return (int) versions.size() + 1; } // "Now" first
    juce::String describeHistoryRow (int row) const;
    juce::TextEditor& getNameField() noexcept { return nameField; }
    juce::TextEditor& getNoteField() noexcept { return noteField; }
    juce::Button& getSaveButton() noexcept { return saveButton; }
    juce::ListBox& getChangesList() noexcept { return changesList; }
    juce::String getChangesHeading() const { return changesHeading.getText(); }

    // When a version was saved, as the history shows it: "now", "5 min", "2 h", the day this week ("Mon"), or the date.
    static juce::String describeWhen (juce::Time saved, juce::Time now);

private:
    class ChangesModel;
    class HistoryModel;

    CommandBridge& commands;
    ProjectContext& context;

    juce::Label title, branchName, changesHeading, historyHeading, emptyText;
    juce::TextEditor nameField, noteField;
    juce::TextButton saveButton;
    std::unique_ptr<ChangesModel> changesModel;
    std::unique_ptr<HistoryModel> historyModel;
    juce::ListBox changesList, historyList;

    std::vector<VersionChange> changes;
    std::vector<VersionInfo> versions;
    std::vector<BranchInfo> branches;

    void refresh(); // what the model holds now
    juce::Colour getBranchColour (const juce::String& branchID) const;
    juce::String getBranchName (const juce::String& branchID) const;

    void projectChanged() override { refresh(); }
    void versionsChanged() override { refresh(); }
    void textEditorTextChanged (juce::TextEditor&) override;
    void textEditorReturnKeyPressed (juce::TextEditor&) override;
};
} // namespace phazite::ui
