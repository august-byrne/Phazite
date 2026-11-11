// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BranchPicker.h"
#include "VersionsView.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/theme/Popups.h"

#include <algorithm>

namespace phazite::ui
{
//==============================================================================
// A commit's message, edited: Enter starts a new line and Ctrl+Enter keeps it, as in the Source Control view's field.
class BranchPicker::MessageField : public juce::TextEditor
{
public:
    std::function<void()> onKeep;

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::commandModifier, 0) && onKeep != nullptr)
        {
            onKeep();
            return true;
        }

        return juce::TextEditor::keyPressed (key);
    }
};

namespace
{
    // How tall the message's field is, in lines, editing one.
    constexpr int messageLines = 4;
} // namespace

BranchPicker::BranchPicker (CommandBridge& bridge, ProjectContext& projectContext, Mode m)
    : commands (bridge),
      context (projectContext),
      mode (m),
      version (m == Mode::message ? getSelectedVersion (projectContext) : juce::String()),
      list ("Branches", this)
{
    const auto current = getCurrentBranch (context);
    const auto onCurrent = current.has_value() ? " into '" + current->name + "'" : juce::String();
    const auto titles = [this, &onCurrent]() -> std::pair<juce::String, juce::String>
    {
        switch (mode)
        {
            case Mode::rename:
                return { "Rename the branch", "The branch's new name" };
            case Mode::merge:
                return { "Merge a branch", "Select a branch to merge" + onCurrent };
            case Mode::remove:
                return { "Delete a branch", "Select a branch to delete" };
            case Mode::message:
                return { "Edit the commit's message", "The commit's message" };
            case Mode::pick:
            default:
                return { "Branches", "Select a branch to checkout, or type a new branch's name" };
        }
    }();

    setTitle (titles.first);
    searchField.setTitle (mode == Mode::pick ? "Search the branches, or name a new one" : titles.second);
    searchField.setTextToShowWhenEmpty (titles.second, getPalette (*this).textMuted);
    searchField.setFont (juce::FontOptions (TextSize::body)); // as VS Code's quick input
    searchField.addListener (this);
    searchField.setWantsKeyboardFocus (true);
    searchField.onNavigationKey = [this] (const juce::KeyPress& key) { return moveSelection (key); };

    // Renaming: the name as it is, to type over.
    if (mode == Mode::rename && current.has_value())
    {
        searchField.setText (current->name, false);
        searchField.selectAll();
    }

    // Editing a message: the field holds it, its first line the name and the rest the note, as the commit's field does.
    if (mode == Mode::message)
    {
        messageField = std::make_unique<MessageField>();
        messageField->setTitle (titles.second);
        messageField->setMultiLine (true, true);
        messageField->setReturnKeyStartsNewLine (true);
        messageField->setScrollbarsShown (true);
        messageField->setFont (juce::FontOptions (TextSize::body));
        messageField->addListener (this);
        messageField->onKeep = [this] { choose (0); };

        for (const auto& info : getVersions (context))
        {
            if (info.id == version)
            {
                messageField->setText (info.hasOwnName          ? info.name + (info.note.isNotEmpty() ? "\n" + info.note : juce::String())
                                       : info.note.isNotEmpty() ? "\n" + info.note
                                                                : juce::String(),
                                       false);
                messageField->setTextToShowWhenEmpty (info.summary, getPalette (*this).textMuted);
            }
        }

        messageField->selectAll();
        addAndMakeVisible (*messageField);
    }
    else
    {
        addAndMakeVisible (searchField);
    }

    // The confirmation before deleting what only a branch holds.
    askToDelete = [] (juce::Component* over, const juce::String& title, const juce::String& message, std::function<void()> onYes)
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle (title)
                                          .withMessage (message)
                                          .withButton ("Delete Branch", 1)
                                          .withButton ("Cancel", 0)
                                          .withAssociatedComponent (over),
                                      [yes = std::move (onYes)] (int chosen)
                                      {
                                          if (chosen == 1)
                                              yes();
                                      });
    };

    list.setRowHeight (Popup::pickerRowHeight);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (list);

    updateRows();
    const auto fieldHeight = mode == Mode::message ? controlHeight * messageLines : controlHeight;
    const auto shownRows = mode == Mode::rename || mode == Mode::message ? 1 : Popup::pickerRows;
    setSize (Popup::pickerWidth, fieldHeight + Space::s + Popup::pickerRowHeight * shownRows + Space::s * 2);
}

BranchPicker::~BranchPicker() = default;

juce::TextEditor& BranchPicker::getSearchField() noexcept
{
    return messageField != nullptr ? static_cast<juce::TextEditor&> (*messageField) : searchField;
}

void BranchPicker::show (CommandBridge& bridge, ProjectContext& projectContext, juce::Component& anchor, Mode mode)
{
    auto picker = std::make_unique<BranchPicker> (bridge, projectContext, mode);
    auto* field = &picker->getSearchField();
    picker->owner = anchor.getTopLevelComponent();

    auto* topLevel = anchor.getTopLevelComponent();
    auto& box =
        juce::CallOutBox::launchAsynchronously (std::move (picker), topLevel->getLocalArea (&anchor, anchor.getLocalBounds()), topLevel);
    box.setArrowSize (0.0f); // a list that hangs from what opened it, as VS Code's quick input, without an arrow

    if (findWindowForPopups (anchor) != nullptr)
        FrostedGlass::putBehind (box, FrostedGlass::Shape::outlined, false); // on frosted glass, as VS Code's quick input

    // Type straight away, once the call-out is on screen.
    juce::MessageManager::callAsync (
        [safeField = juce::Component::SafePointer<juce::TextEditor> (field)]
        {
            if (safeField != nullptr)
                safeField->grabKeyboardFocus();
        });
}

void BranchPicker::resized()
{
    auto area = getLocalBounds().reduced (Space::s);
    getSearchField().setBounds (area.removeFromTop (mode == Mode::message ? controlHeight * messageLines : controlHeight));
    area.removeFromTop (Space::s);
    list.setBounds (area);
}

void BranchPicker::addBranchRows (Row::Kind kind, const juce::String& typed)
{
    // The other branches whose names have what was typed, each with its last commit and when it was made; merging, how many of
    // its commits the current branch doesn't have, and not one that has none; deleting, not one a merge under way needs.
    const auto current = getCurrentBranch (context);
    const auto versions = getVersions (context);
    const auto now = juce::Time::getCurrentTime();

    for (const auto& branch : getBranches (context))
    {
        if ((current.has_value() && branch.id == current->id) || (typed.isNotEmpty() && ! branch.name.containsIgnoreCase (typed)))
            continue;

        const auto head = std::find_if (versions.begin(), versions.end(), [&branch] (const VersionInfo& v) { return v.id == branch.head; });
        const auto last =
            head != versions.end() ? head->name + ", " + VersionsView::describeWhen (head->saved, now) : juce::String ("no commits yet");
        Row row { kind, branch, branch.name, last, false, true };

        if (kind == Row::Kind::merge)
        {
            const auto toMerge = countCommitsToMerge (context, branch.id);
            row.enabled = canMergeBranch (context, branch.id);
            row.about = toMerge == 0 ? juce::String ("nothing to merge")
                                     : juce::String (toMerge) + (toMerge == 1 ? " commit" : " commits") + " to merge; last " + last;
        }
        else
        {
            row.enabled = canDeleteBranch (context, branch.id);
        }

        rows.push_back (std::move (row));
    }
}

void BranchPicker::updateRows()
{
    rows.clear();
    const auto typed = getSearchField().getText().trim();
    const auto name = toBranchName (typed); // as git keeps it
    const auto current = getCurrentBranch (context);
    const auto branches = getBranches (context);
    const auto isTaken = [&branches, &name] (const juce::String& except)
    {
        return std::any_of (branches.begin(), branches.end(),
                            [&] (const BranchInfo& branch) { return branch.id != except && branch.name.equalsIgnoreCase (name); });
    };

    const auto finish = [this] (int selected)
    {
        list.updateContent();
        list.selectRow (selected);
        list.repaint();
    };

    if (mode == Mode::message)
    {
        // The one row: the message kept, as typed (an empty first line leaves the commit named by what it changed).
        const auto versions = getVersions (context);
        const auto exists = std::any_of (versions.begin(), versions.end(), [this] (const VersionInfo& info) { return info.id == version; });
        Row row { Row::Kind::message, {}, "Save the commit's message", {}, false, exists };
        row.about = formatKey (juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::commandModifier, 0).getTextDescription())
                        .replace ("Return", "Enter");
        rows.push_back (std::move (row));
        finish (0);
        return;
    }

    if (mode == Mode::rename)
    {
        // The one row: the name typed, which no other branch may have.
        if (current.has_value())
        {
            const auto taken = isTaken (current->id);
            Row row { Row::Kind::rename, *current, {}, {}, false, name.isNotEmpty() && ! taken && name != current->name };
            row.name = name.isEmpty() ? juce::String ("Type the branch's new name") : "Rename '" + current->name + "' to '" + name + "'";
            row.about = taken ? juce::String ("another branch has that name") : juce::String();
            rows.push_back (std::move (row));
        }

        finish (0);
        return;
    }

    if (mode == Mode::merge || mode == Mode::remove)
    {
        addBranchRows (mode == Mode::merge ? Row::Kind::merge : Row::Kind::remove, typed);

        // Enter takes the first one that can be.
        const auto first = std::find_if (rows.begin(), rows.end(), [] (const Row& row) { return row.enabled; });
        finish (first != rows.end() ? (int) std::distance (rows.begin(), first) : 0);
        return;
    }

    // Create new branch, as VS Code's first item, named as typed, unless a branch has that name: Enter on its name checks it
    // out instead.
    if (! isTaken ({}))
    {
        const auto last = getLastVersion (context);
        Row row {
            Row::Kind::newBranch, {}, {}, {}, false, last.has_value() && ! isSavingVersion (context) && ! isReadingVersions (context)
        };
        row.name = name.isEmpty() ? juce::String ("Create new branch...") : "Create new branch '" + name + "'";
        row.about = last.has_value() ? juce::String ("from the current commit, with the changes") : juce::String ("no commit yet");
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
        if (typed.isNotEmpty() && ! branch.name.containsIgnoreCase (typed) && ! branch.name.containsIgnoreCase (name))
            continue;

        const auto isCurrent = current.has_value() && branch.id == current->id;
        Row row { Row::Kind::branch, branch, branch.name, {}, isCurrent, isCurrent || canSwitchBranch (context) };

        if (isCurrent)
        {
            const auto changes = countChanges (context);
            row.about =
                "current" + (changes == 0 ? juce::String() : ", " + juce::String (changes) + (changes == 1 ? " change" : " changes"));
        }
        else
        {
            const auto head =
                std::find_if (versions.begin(), versions.end(), [&branch] (const VersionInfo& v) { return v.id == branch.head; });
            row.about = head != versions.end() ? head->name + ", " + VersionsView::describeWhen (head->saved, now)
                                               : juce::String ("no commits yet");
        }

        if (firstMatch < 0)
            firstMatch = (int) rows.size();

        rows.push_back (std::move (row));
    }

    // What Enter does: with a name typed, the first branch it matches, else the new branch; with none, the new branch.
    finish (typed.isNotEmpty() && firstMatch >= 0 ? firstMatch : 0);
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
    const auto typed = getSearchField().getText().trim();
    const auto message = getSearchField().getText();
    const auto commit = version;
    const auto ask = askToDelete;
    const auto window = owner;
    auto& bridge = commands;
    auto& projectContext = context;
    close();

    juce::MessageManager::callAsync (
        [&bridge, &projectContext, row, typed, message, commit, ask, window]
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

                case Row::Kind::merge:
                    mergeBranch (projectContext, row.branch.id);
                    break;

                case Row::Kind::remove:
                {
                    // At once while nothing goes with it but its name; else after the question.
                    const auto id = row.branch.id;

                    if (const auto goes = describeBranchDeletion (projectContext, id); goes.isEmpty() || ask == nullptr)
                        deleteBranch (projectContext, id);
                    else
                        ask (window.getComponent(), "Delete the branch '" + row.branch.name + "'?", goes,
                             [&projectContext, id] { deleteBranch (projectContext, id); });

                    break;
                }

                case Row::Kind::message:
                    // Its first line the name, the lines after it the note.
                    editVersionMessage (projectContext, commit, message.upToFirstOccurrenceOf ("\n", false, false).trim(),
                                        message.fromFirstOccurrenceOf ("\n", false, false).trim());
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

    // Its icon: a plus for a new branch, a pencil for a name or a message, else the branch's in its color; and a tick on the
    // one the song is on.
    const auto iconArea = [&area] (bool fromLeft)
    {
        const auto column =
            fromLeft ? area.removeFromLeft ((int) Popup::pickerIconSize) : area.removeFromRight ((int) Popup::pickerIconSize);
        return column.toFloat().withSizeKeepingCentre (Popup::pickerIconSize, Popup::pickerIconSize);
    };

    const auto isBranch = row.kind == Row::Kind::branch || row.kind == Row::Kind::merge || row.kind == Row::Kind::remove;
    auto& icon = row.kind == Row::Kind::newBranch                                  ? plusIcon
                 : row.kind == Row::Kind::rename || row.kind == Row::Kind::message ? nameIcon
                                                                                   : branchIcon;
    icon.draw (g, iconArea (true), isBranch && ! selected ? row.branch.colour.withMultipliedAlpha (alpha) : textColour);
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
