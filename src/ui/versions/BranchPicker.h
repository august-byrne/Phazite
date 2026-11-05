// SPDX-License-Identifier: AGPL-3.0-or-later
// The branch picker (docs/versions.md: Branches), as VS Code's branch quick pick: first New Branch from Now, named as typed
// (else "Idea 2" and on), then the project's branches, the one the song is on ticked, each with what it holds (its last
// version and when it was saved; for the one the song is on, its changes since). Typing narrows the branches; Enter or a
// click switches to the one picked (version.switchBranch) or makes the new one (version.newBranch). Or, to rename the
// branch the song is on, its name to type over. In a call-out like the command palette's; Esc or a click elsewhere closes it.
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
        rename
    };

    BranchPicker (CommandBridge&, ProjectContext&, Mode);

    // Opens a picker in a call-out pointing at anchor (the title bar's command search, where VS Code's pickers open).
    static void show (CommandBridge&, ProjectContext&, juce::Component& anchor, Mode);

    void resized() override;

    // For tests: the rows, as screen readers say them; the search field; and a row chosen, as Enter on it chooses it.
    int getNumRows() override { return (int) rows.size(); }
    juce::String getNameForRow (int row) override;
    juce::TextEditor& getSearchField() noexcept { return searchField; }
    void choose (int row);

private:
    // What a row does: makes a branch from Now with the name typed, switches to a branch, or names the current one anew.
    struct Row
    {
        enum class Kind
        {
            newBranch,
            branch,
            rename
        };

        Kind kind = Kind::branch;
        BranchInfo branch;
        juce::String name, about; // what it says, and what is muted after it
        bool current = false, enabled = true;
    };

    CommandBridge& commands;
    ProjectContext& context;
    const Mode mode;
    SearchField searchField;
    juce::ListBox list;
    std::vector<Row> rows;
    TintedIcon branchIcon { Icon::versions }, plusIcon { Icon::plus }, nameIcon { Icon::draw }, checkIcon { Icon::check };

    void updateRows();
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
