// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BranchPicker.h"
#include "VersionsView.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>

namespace phazite::ui
{
BranchPicker::BranchPicker (CommandBridge& bridge, ProjectContext& projectContext, Mode m)
    : commands (bridge),
      context (projectContext),
      mode (m),
      list ("Branches", this)
{
    const auto renaming = mode == Mode::rename;
    setTitle (renaming ? "Rename the branch" : "Branches");
    getProperties().set (LookAndFeel::popupListProperty, true); // translucent, as menus are

    searchField.setTitle (renaming ? "The branch's new name" : "Search the branches, or name a new one");
    searchField.setTextToShowWhenEmpty (renaming ? "The branch's new name" : "Select a branch to switch to, or type a new branch's name",
                                        getPalette (*this).textMuted);
    searchField.setFont (juce::FontOptions (TextSize::title));
    searchField.addListener (this);
    searchField.setWantsKeyboardFocus (true);
    searchField.onNavigationKey = [this] (const juce::KeyPress& key) { return moveSelection (key); };

    // Renaming: the name as it is, to type over.
    if (const auto current = getCurrentBranch (context); renaming && current.has_value())
    {
        searchField.setText (current->name, false);
        searchField.selectAll();
    }

    list.setRowHeight (Popup::pickerRowHeight);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

    addAndMakeVisible (searchField);
    addAndMakeVisible (list);

    updateRows();
    setSize (Popup::pickerWidth, controlHeight + Space::s + Popup::pickerRowHeight * (renaming ? 1 : Popup::pickerRows) + Space::s * 2);
}

void BranchPicker::show (CommandBridge& bridge, ProjectContext& projectContext, juce::Component& anchor, Mode mode)
{
    auto picker = std::make_unique<BranchPicker> (bridge, projectContext, mode);
    auto* searchField = &picker->searchField;

    auto* topLevel = anchor.getTopLevelComponent();
    juce::CallOutBox::launchAsynchronously (std::move (picker), topLevel->getLocalArea (&anchor, anchor.getLocalBounds()), topLevel);

    // Type straight away, once the call-out is on screen.
    juce::MessageManager::callAsync (
        [field = juce::Component::SafePointer<juce::TextEditor> (searchField)]
        {
            if (field != nullptr)
                field->grabKeyboardFocus();
        });
}

void BranchPicker::resized()
{
    auto area = getLocalBounds().reduced (Space::s);
    searchField.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::s);
    list.setBounds (area);
}

void BranchPicker::updateRows()
{
    rows.clear();
    const auto typed = searchField.getText().trim();
    const auto current = getCurrentBranch (context);
    const auto branches = getBranches (context);
    const auto isTaken = [&branches, &typed] (const juce::String& except)
    {
        return std::any_of (branches.begin(), branches.end(),
                            [&] (const BranchInfo& branch) { return branch.id != except && branch.name.equalsIgnoreCase (typed); });
    };

    if (mode == Mode::rename)
    {
        // The one row: the name typed, which no other branch may have.
        if (current.has_value())
        {
            const auto taken = isTaken (current->id);
            Row row { Row::Kind::rename, *current, {}, {}, false, typed.isNotEmpty() && ! taken && typed != current->name };
            row.name =
                typed.isEmpty() ? juce::String ("Type the branch's new name") : "Rename \"" + current->name + "\" to \"" + typed + "\"";
            row.about = taken ? juce::String ("another branch has that name") : juce::String();
            rows.push_back (std::move (row));
        }

        list.updateContent();
        list.selectRow (0);
        list.repaint();
        return;
    }

    // New Branch from Now, named as typed, unless a branch has that name: Enter on its name switches to it instead.
    if (! isTaken ({}))
    {
        const auto last = getLastVersion (context);
        Row row {
            Row::Kind::newBranch, {}, {}, {}, false, last.has_value() && ! isSavingVersion (context) && ! isReadingVersions (context)
        };
        row.name = typed.isEmpty() ? juce::String ("New Branch from Now") : "New Branch \"" + typed + "\" from Now";
        row.about = last.has_value() ? "from \"" + last->name + "\", with the changes since" : juce::String ("save a version first");
        rows.push_back (std::move (row));
    }

    // The branches whose names have what was typed, the one the song is on first.
    const auto versions = getVersions (context);
    const auto now = juce::Time::getCurrentTime();
    auto ordered = branches;
    std::stable_partition (ordered.begin(), ordered.end(),
                           [&current] (const BranchInfo& branch) { return current.has_value() && branch.id == current->id; });
    auto firstMatch = -1;

    for (const auto& branch : ordered)
    {
        if (typed.isNotEmpty() && ! branch.name.containsIgnoreCase (typed))
            continue;

        const auto isCurrent = current.has_value() && branch.id == current->id;
        Row row { Row::Kind::branch, branch, branch.name, {}, isCurrent, isCurrent || canSwitchBranch (context) };

        if (isCurrent)
        {
            const auto changes = (int) getChangesSinceLastVersion (context).size();
            row.about =
                "the song is on it"
                + (changes == 0 ? juce::String (", no changes") : ", " + juce::String (changes) + (changes == 1 ? " change" : " changes"));
        }
        else
        {
            const auto head =
                std::find_if (versions.begin(), versions.end(), [&branch] (const VersionInfo& v) { return v.id == branch.head; });
            row.about = head != versions.end() ? head->name + ", " + VersionsView::describeWhen (head->saved, now)
                                               : juce::String ("no versions yet");
        }

        if (firstMatch < 0)
            firstMatch = (int) rows.size();

        rows.push_back (std::move (row));
    }

    // What Enter does: with a name typed, the first branch it matches, else the new branch; with none, the new branch.
    list.updateContent();
    list.selectRow (typed.isNotEmpty() && firstMatch >= 0 ? firstMatch : 0);
    list.repaint();
}

bool BranchPicker::moveSelection (const juce::KeyPress& key)
{
    // The search field keeps the focus; the arrow keys move through the list.
    const auto row = list.getSelectedRow();

    if (key == juce::KeyPress::downKey)
    {
        list.selectRow (juce::jmin (row + 1, (int) rows.size() - 1));
        return true;
    }

    if (key == juce::KeyPress::upKey)
    {
        list.selectRow (juce::jmax (row - 1, 0));
        return true;
    }

    return false;
}

void BranchPicker::choose (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) rows.size()) || ! rows[(size_t) index].enabled)
        return;

    // Closing deletes this picker, so take what is needed first; then act once it has gone (the keyboard back with the
    // window), as the command palette runs its command.
    const auto row = rows[(size_t) index];
    const auto typed = searchField.getText().trim();
    auto& bridge = commands;
    auto& projectContext = context;
    close();

    juce::MessageManager::callAsync (
        [&bridge, &projectContext, row, typed]
        {
            switch (row.kind)
            {
                case Row::Kind::newBranch:
                    // From Now, named as typed.
                    selectVersion (projectContext, {});

                    if (bridge.invoke ("version.newBranch") && typed.isNotEmpty())
                        if (const auto made = getCurrentBranch (projectContext))
                            renameBranch (projectContext, made->id, typed);

                    break;

                case Row::Kind::branch:
                    if (! row.current)
                    {
                        selectBranch (projectContext, row.branch.id);
                        bridge.invoke ("version.switchBranch");
                    }

                    break;

                case Row::Kind::rename:
                    renameBranch (projectContext, row.branch.id, typed);
                    break;
            }
        });
}

void BranchPicker::close()
{
    if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
        box->dismiss();
}

void BranchPicker::paintListBoxItem (int index, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (index, (int) rows.size()))
        return;

    const auto& p = getPalette (*this);
    const auto& row = rows[(size_t) index];
    auto area = juce::Rectangle<int> (width, height);

    if (selected)
    {
        g.setColour (p.accent);
        g.fillRoundedRectangle (area.toFloat().reduced (1.0f), cornerRadius);
    }

    const auto alpha = row.enabled ? 1.0f : 0.5f;
    const auto textColour = (selected ? p.textOnAccent : p.text).withMultipliedAlpha (alpha);
    const auto mutedColour = (selected ? p.textOnAccent : p.textMuted).withMultipliedAlpha (alpha);
    area.reduce (Space::s, 0);

    // Its icon: a plus for a new branch, a pencil for a name, else the branch's in its color; and a tick on the one the song is on.
    const auto iconArea = [&area] (bool fromLeft)
    {
        const auto column =
            fromLeft ? area.removeFromLeft ((int) Popup::pickerIconSize) : area.removeFromRight ((int) Popup::pickerIconSize);
        return column.toFloat().withSizeKeepingCentre (Popup::pickerIconSize, Popup::pickerIconSize);
    };

    auto& icon = row.kind == Row::Kind::newBranch ? plusIcon : row.kind == Row::Kind::rename ? nameIcon : branchIcon;
    icon.draw (g, iconArea (true),
               row.kind == Row::Kind::branch && ! selected ? row.branch.colour.withMultipliedAlpha (alpha) : textColour);
    area.removeFromLeft (Space::s);

    if (row.current)
    {
        checkIcon.draw (g, iconArea (false), textColour);
        area.removeFromRight (Space::s);
    }

    // Its name, then what it holds, muted.
    g.setColour (textColour);
    g.setFont (juce::FontOptions (TextSize::body, row.current ? juce::Font::bold : juce::Font::plain));
    const auto nameWidth =
        juce::jmin (area.getWidth(), juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), row.name)) + Space::m);
    g.drawText (row.name, area.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);

    g.setColour (mutedColour);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (row.about, area, juce::Justification::centredLeft, true);
}

juce::String BranchPicker::getNameForRow (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) rows.size()))
        return {};

    const auto& row = rows[(size_t) index];
    return row.name + (row.about.isNotEmpty() ? ", " + row.about : juce::String()) + (row.enabled ? "" : ", not now");
}
} // namespace phazite::ui
