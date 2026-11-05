// SPDX-License-Identifier: AGPL-3.0-or-later
// The project's versions, in the left panel (docs/versions.md: The Versions view), laid out as VS Code's Source Control view:
// - at the top, the branch the song is on, whose button opens the branch picker (version.pickBranch), and a menu of what
//   else the view does;
// - the message for the next version, whose first line is its name and the lines after it its note, both optional
//   (Ctrl+Enter saves), then Save Version across the view, whose menu's part holds Bounce Each Version;
// - CHANGES: what changed since the current branch's last version, a row for each track (the master, the song's own),
//   each with its icon, what changed in it, and its letter at its end, A added, M changed, D taken away (its name struck
//   through), in VS Code's colors for them; under the pointer, its Put Back. A click selects its track. Or, while a merge's
//   list shows, what the other line changed, ticked to take it; while a merge waits for its version, its clashes, each
//   switched between its values;
// - HISTORY: every branch's versions newest first, each on its branch's line in its color, a branch's name on its last
//   version, with a play mark on those with a bounce, which hears it in place of the song (Now's, the song again);
// - and the room the versions take.
// Both sections fold under their headings. By the model's functions (model/versions/VersionsModel.h) and the versions'
// commands.
#pragma once

#include "model/project/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "services/Settings.h"
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

    // The keyboard to the version's message, as showing the view gives it the keyboard.
    void focusMessage();
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

    // For tests: what the lists show, a row at a time, as screen readers say it; a change's letter (A, M or D); and the parts.
    Lines getLines() const noexcept { return lines; }
    int getNumChanges() const noexcept;
    juce::String describeChange (int row) const;
    juce::String getChangeLetter (int row) const;
    int getNumHistoryRows() const noexcept { return (int) versions.size() + 1; } // "Now" first
    juce::String describeHistoryRow (int row) const;
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
    juce::String getChangesHeading() const;
    juce::String getRoomText() const { return roomText.getText(); }

    // The menus of Save Version's part and of the view's three dots, as their buttons show them.
    juce::PopupMenu makeSaveMenu();
    juce::PopupMenu makeMoreMenu();

    // Where a history row's play mark is, in the history list, while any version has a bounce (and the row has one); and a
    // change's Put Back, in the changes list, which shows on the row under the pointer and on the selected one.
    juce::Rectangle<int> getPlayMarkPosition (int row) const;
    juce::Rectangle<int> getPutBackPosition (int row) const;

    // When a version was saved, as the history shows it: "now", "5 min", "2 h", the day this week ("Mon"), or the date.
    static juce::String describeWhen (juce::Time saved, juce::Time now);

private:
    class ChangesModel;
    class HistoryModel;
    class MessageField;
    class BranchButton;
    class Section;
    struct RowHover;

    CommandBridge& commands;
    ProjectContext& context;
    Settings& settings;

    juce::Label title, emptyText, roomText;
    std::unique_ptr<BranchButton> branchButton;
    IconButton moreButton { "More", Icon::moreActions };
    std::unique_ptr<MessageField> messageField;
    IconButton saveButton { "Save Version", Icon::check }, saveMenuButton { "Save Version's choices", Icon::expanded };
    std::unique_ptr<Section> changesSection, historySection;
    juce::TextButton doneButton;                // from another version's differences back to the changes
    juce::TextButton mergeButton, cancelButton; // a merge's list's
    std::unique_ptr<ChangesModel> changesModel;
    std::unique_ptr<HistoryModel> historyModel;
    juce::ListBox changesList, historyList;
    std::unique_ptr<RowHover> changesHover;

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

    // The changes' icons (a track's, the master's, the song's) and Put Back's, drawn in each row's colors.
    TintedIcon trackIcon { Icon::sound }, masterIcon { Icon::effect }, songIcon { Icon::song }, putBackIcon { Icon::undo };

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
    // The message as typed, the model's draft: its first line the name, the rest the note. And the draft as one message, and
    // as it was when the view last took or showed it, so that the view shows the draft again only when it changed in the model.
    void takeMessage();
    juce::String getDraftMessage() const;
    juce::String takenMessage;
    void showMenu (std::initializer_list<const char*> commandIDs); // for the row under the pointer, which is selected
    void showClashMenu (int clash);
    bool showsPutBack (int row) const;                // under the pointer, or selected
    int getLane (const juce::String& branchID) const; // the branch's line's column in the history
    juce::Colour getBranchColour (const juce::String& branchID) const;
    juce::String getBranchName (const juce::String& branchID) const;

    void projectChanged() override { refresh(); }
    void versionsChanged() override { refresh(); }
    void textEditorTextChanged (juce::TextEditor&) override;
};
} // namespace phazite::ui
