// SPDX-License-Identifier: AGPL-3.0-or-later
// Source control's quick inputs (docs/versions.md: Branches), as VS Code's branch quick picks, in a call-out like the command
// palette's; Esc or a click elsewhere closes one:
// - Checkout to...: first Create new branch, named as typed (else "idea-2" and on), then the project's branches, the one the
//   song is on ticked, each with what it holds (its last commit and when it was made; for the one the song is on, its
//   changes since). Typing narrows the branches; Enter or a click checks out the one picked (version.switchBranch) or makes
//   the new one (version.newBranch).
// - Rename Branch...: the name of the branch the song is on, to type over.
// - Merge Branch...: the other branches, each with how many of its commits the current branch doesn't have; the one picked
//   is merged in (its lines listed to tick in the Source Control view).
// - Delete Branch...: the other branches; the one picked is deleted, after a question when it would take commits no other
//   branch has, or changes left on it, with it.
// - Edit Commit Message...: the selected commit's message, its first line the name and the lines after it the note, to type
//   over; Ctrl+Enter, or a click on its row, keeps it.
#pragma once

#include "model/project/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "ui/components/Controls.h"
#include "ui/input/CommandBridge.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class BranchPicker : public juce::Component,
                     private juce::TextEditor::Listener,
                     private juce::ListBoxModel
{
public:
    enum class Mode
    {
        pick,
        rename,
        merge,
        remove,
        message
    };

    BranchPicker (CommandBridge&, ProjectContext&, Mode);
    ~BranchPicker() override;

    // Opens a picker in a call-out pointing at anchor (the title bar's command search, where VS Code's pickers open).
    static void show (CommandBridge&, ProjectContext&, juce::Component& anchor, Mode);

    void resized() override;

    // For tests: the rows, as screen readers say them; the field typed into (the message's, editing one); and a row chosen,
    // as Enter on it chooses it.
    int getNumRows() override { return (int) rows.size(); }
    juce::String getNameForRow (int row) override;
    juce::TextEditor& getSearchField() noexcept;
    void choose (int row);

    // The question asked before a branch is deleted with what only it holds (over the window the picker opened over, its
    // title, what goes, and what runs if the answer is yes): an alert window, which tests replace. It is asked once the
    // picker has gone.
    using Question =
        std::function<void (juce::Component* over, const juce::String& title, const juce::String& message, std::function<void()> onYes)>;
    Question askToDelete;

private:
    // What a row does: makes a branch from Now with the name typed, switches to a branch, names the current one anew, merges
    // a branch in, deletes one, or keeps a commit's message.
    struct Row
    {
        enum class Kind
        {
            newBranch,
            branch,
            rename,
            merge,
            remove,
            message
        };

        Kind kind = Kind::branch;
        BranchInfo branch;
        juce::String name, about; // what it says, and what is muted after it
        bool current = false, enabled = true;
    };

    class MessageField;

    CommandBridge& commands;
    ProjectContext& context;
    const Mode mode;
    const juce::String version;                          // the commit whose message is edited
    juce::Component::SafePointer<juce::Component> owner; // the window it opened over, which a question opens over too
    SearchField searchField;
    std::unique_ptr<MessageField> messageField;
    juce::ListBox list;
    std::vector<Row> rows;
    TintedIcon branchIcon { Icon::versions }, plusIcon { Icon::plus }, nameIcon { Icon::draw }, checkIcon { Icon::check };

    void updateRows();
    void addBranchRows (Row::Kind, const juce::String& typed);
    bool moveSelection (const juce::KeyPress&);
    void close();

    void textEditorTextChanged (juce::TextEditor&) override { updateRows(); }
    void textEditorReturnKeyPressed (juce::TextEditor&) override { choose (list.getSelectedRow()); }
    void textEditorEscapeKeyPressed (juce::TextEditor&) override { close(); }

    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override { choose (row); }
    void returnKeyPressed (int row) override { choose (row); }
};
} // namespace phazite::ui
