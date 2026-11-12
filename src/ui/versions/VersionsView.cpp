// SPDX-License-Identifier: AGPL-3.0-or-later
#include "VersionsView.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <map>

namespace phazite::ui
{
namespace
{
    ChangeKind getKind (const VersionChange& change)
    {
        return change.kind == VersionChange::Kind::added     ? ChangeKind::added
               : change.kind == VersionChange::Kind::removed ? ChangeKind::removed
                                                             : ChangeKind::changed;
    }

    // A line's letter, as VS Code marks what git finds: U untracked (added, not staged yet), A added, M modified, D deleted.
    juce::String getLetter (const VersionChange& change)
    {
        if (change.kind == VersionChange::Kind::added)
            return change.staged ? "A" : "U";

        return change.kind == VersionChange::Kind::removed ? "D" : "M";
    }

    // A line as screen readers say it, its status as VS Code names it: "Bass, Modified: volume", "Keys, Untracked", "Lead,
    // Index Deleted".
    juce::String describeChangeLine (const VersionChange& change)
    {
        const juce::String status = change.kind == VersionChange::Kind::added     ? (change.staged ? "Index Added" : "Untracked")
                                    : change.kind == VersionChange::Kind::removed ? (change.staged ? "Index Deleted" : "Deleted")
                                                                                  : (change.staged ? "Index Modified" : "Modified");
        return change.name + ", " + status + (change.what.isEmpty() ? juce::String() : ": " + change.what);
    }

    // A conflict's value, as a merge editor names whose it is: the base's, the current branch's, or the incoming one's.
    juce::String describeValue (const ClashInfo& clash, int value)
    {
        const juce::String whose[] = { "Base", "Current", "Incoming (" + clash.otherBranch + ")" };
        return whose[value] + ": " + clash.values[value];
    }

    ThemeName getThemeOf (const juce::Component& component)
    {
        if (const auto* lookAndFeel = dynamic_cast<const LookAndFeel*> (&component.getLookAndFeel()))
            return lookAndFeel->getTheme();

        return ThemeName::dark;
    }

    // A row's name and what it says of it, side by side, the name struck through for what was deleted.
    void drawLine (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour nameColour, juce::Colour whatColour, const juce::String& name,
                   const juce::String& what, bool struckThrough = false)
    {
        g.setColour (nameColour);
        g.setFont (juce::FontOptions (TextSize::body));
        const auto nameWidth =
            juce::jmin (area.getWidth(), juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), name)) + Space::s);
        const auto nameArea = area.removeFromLeft (nameWidth);
        g.drawText (name, nameArea, juce::Justification::centredLeft, true);

        if (struckThrough)
            g.fillRect (juce::Rectangle<float> ((float) nameArea.getX(), (float) nameArea.getCentreY(),
                                                (float) nameWidth - (float) Space::s, History::strikethroughThickness));

        g.setColour (whatColour);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (what, area, juce::Justification::centredLeft, true);
    }

    void drawLetter (juce::Graphics& g, juce::Rectangle<int>& area, const juce::String& letter, juce::Colour colour)
    {
        g.setColour (colour);
        g.setFont (juce::FontOptions (History::letterSize, juce::Font::bold));
        g.drawText (letter, area.removeFromRight (History::letterWidth), juce::Justification::centred, false);
    }

    // A change's row in its list, which is as wide as the panel: from its icon's column, one level into its group, to its
    // letter's end, inside the row's inset (VS Code's Source Control, SideBar).
    juce::Rectangle<int> getRowContent (int width, int height)
    {
        return juce::Rectangle<int> (width, height).withTrimmedLeft (History::rowIcon).withTrimmedRight (Space::s);
    }

    // The column of a row's icon, where a chevron would be, and the gap after it.
    juce::Rectangle<float> takeIconColumn (juce::Rectangle<int>& area, float iconSize)
    {
        const auto column = area.removeFromLeft (SideBar::chevronSize).toFloat().withSizeKeepingCentre (iconSize, iconSize);
        area.removeFromLeft (SideBar::gap);
        return column;
    }

} // namespace

juce::String VersionsView::describeWhen (juce::Time saved, juce::Time now)
{
    const auto elapsed = now - saved;

    if (elapsed.inMinutes() < 1.0)
        return "now";

    if (elapsed.inMinutes() < 60.0)
        return juce::String ((int) elapsed.inMinutes()) + " min";

    if (elapsed.inHours() < 24.0)
        return juce::String ((int) elapsed.inHours()) + " h";

    if (elapsed.inDays() < 7.0)
        return saved.getWeekdayName (true);

    return juce::String (saved.getDayOfMonth()) + " " + saved.getMonthName (true)
           + (saved.getYear() != now.getYear() ? " " + juce::String (saved.getYear()) : juce::String());
}

//==============================================================================
// The commit's message: Enter starts a new line, Ctrl+Enter commits, as in VS Code.
class VersionsView::MessageField : public juce::TextEditor
{
public:
    std::function<void()> onSave;

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::commandModifier, 0) && onSave != nullptr)
        {
            onSave();
            return true;
        }

        return juce::TextEditor::keyPressed (key);
    }
};

//==============================================================================
// While a commit is checked out on its own (Checkout (Detached)), under the branch: what the song is, and the two ways on, as
// Figma's and Google Docs' version history say where they are and offer the way back (the research of 2026-11-12): Back to
// the branch it came from, the main button, and Create Branch Here, which keeps what changes.
class VersionsView::DetachedBar : public juce::Component
{
public:
    explicit DetachedBar (VersionsView& v)
        : view (v)
    {
        setTitle ("Checked out");
        text.setFont (juce::FontOptions (TextSize::small));
        text.setJustificationType (juce::Justification::topLeft);
        text.setMinimumHorizontalScale (1.0f);
        text.setInterceptsMouseClicks (false, false);
        setThemeColour (text, juce::Label::textColourId, &Palette::text);
        addAndMakeVisible (text);

        backButton.setTooltip ("Makes the song the branch's again, as it was left there. What changed here goes into Local History");
        backButton.onClick = [this] { view.commands.invoke ("version.backToBranch"); };
        setThemeColour (backButton, juce::TextButton::buttonColourId, &Palette::accent);
        setThemeColour (backButton, juce::TextButton::textColourOffId, &Palette::textOnAccent);
        addAndMakeVisible (backButton);

        branchButton.setTooltip ("Creates a branch from this commit, keeping what changed here, to commit it");
        branchButton.onClick = [this]
        {
            selectVersion (view.context, {}); // here, as the song is now
            view.commands.invoke ("version.newBranch");
        };
        addAndMakeVisible (branchButton);
    }

    void show (const BranchInfo& checkout, const juce::String& backText)
    {
        text.setText ("Checked out \"" + checkout.name + "\", on no branch: it plays and can change, but nothing is committed here.",
                      juce::dontSendNotification);
        backButton.setButtonText (backText);
        backButton.setTitle (backText);
    }

    int getPreferredHeight (int width) const
    {
        juce::AttributedString attributed (text.getText());
        attributed.setFont (text.getFont());
        juce::TextLayout layout;
        layout.createLayout (attributed, (float) juce::jmax (1, width - 2 * Space::s));
        return juce::roundToInt (layout.getHeight()) + 2 * Space::xs + Space::xs + SideBar::controlHeight + Space::s;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        g.setColour (p.selected);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (Space::s, Space::xs);
        auto buttons = area.removeFromBottom (SideBar::controlHeight);
        area.removeFromBottom (Space::xs);
        text.setBounds (area);
        backButton.setBounds (buttons.removeFromLeft ((buttons.getWidth() - Space::xs) / 2));
        buttons.removeFromLeft (Space::xs);
        branchButton.setBounds (buttons);
    }

    juce::TextButton backButton { "Back to Branch" }, branchButton { "Create Branch Here" };

private:
    VersionsView& view;
    juce::Label text;
};

//==============================================================================
// The branch the song is on, as VS Code's status bar names it: its icon and name in its color, and a chevron; a click opens
// the branch picker.
class VersionsView::BranchButton : public juce::Button
{
public:
    BranchButton()
        : juce::Button ("Branch")
    {
        setTitle ("Branch");
    }

    void setBranch (const juce::String& name, juce::Colour newColour)
    {
        setButtonText (name);
        colour = newColour;
        repaint();
    }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        const auto& p = getPalette (*this);

        if (isHighlighted || isDown)
        {
            g.setColour (isDown ? p.selected : p.hover);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);
        }

        // Its icon where a first level's chevron is (it sits in the row's inset).
        auto area = getLocalBounds().withTrimmedLeft (SideBar::rowChevron - (int) ListRow::inset).withTrimmedRight (Space::xs);
        const auto iconArea = [&area]
        {
            return area.removeFromLeft ((int) History::rowIconSize)
                .toFloat()
                .withSizeKeepingCentre (History::rowIconSize, History::rowIconSize);
        };

        // Its icon, its name, and the chevron right after the name.
        icon.draw (g, iconArea(), colour);
        area.removeFromLeft (Space::xs);
        g.setColour (colour);
        g.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
        const auto nameWidth =
            juce::jmin (area.getWidth() - (int) History::rowIconSize,
                        juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), getButtonText())) + Space::xs);
        g.drawText (getButtonText(), area.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);
        chevron.draw (g, iconArea(), p.textMuted);
    }

private:
    juce::Colour colour;
    TintedIcon icon { Icon::versions }, chevron { Icon::expanded };
};

//==============================================================================
// A list of lines: the staged changes, each to unstage; or the working tree's, each to discard or stage, a click selecting a
// line's track; a merge's lines, a click ticking one; or a merge's conflicts, a click choosing a value.
class VersionsView::ChangesModel : public juce::ListBoxModel
{
public:
    ChangesModel (VersionsView& v, bool forStaged)
        : view (v),
          isStaged (forStaged)
    {
    }

    int getNumRows() override { return isStaged ? view.getNumStaged() : view.getNumChanges(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;

        const auto& p = getPalette (view);
        const auto theme = getThemeOf (view);
        drawListRow (g, view, juce::Rectangle<int> (width, height).toFloat(), selected, row == getHover().row);
        auto area = getRowContent (width, height);

        if (! isStaged && view.lines == Lines::merge)
        {
            // A box ticked while the merge takes the line, its letter, and a word where it conflicts with this branch's change.
            const auto& line = view.mergeLines[(size_t) row];
            const auto colour = getChangeColour (getKind (line.change), theme);
            const auto box = takeIconColumn (area, History::tickSize);
            g.setColour (line.ticked ? p.accent : p.textMuted);
            g.drawRoundedRectangle (box.reduced (0.5f), cornerRadius / 2.0f, 1.0f);

            if (line.ticked)
                g.fillRoundedRectangle (box.reduced (History::tickInset), cornerRadius / 2.0f);

            drawLetter (g, area, getLetter (line.change), colour);

            if (line.clashes)
            {
                g.setColour (p.warning);
                g.setFont (juce::FontOptions (TextSize::small));
                const juce::String word ("conflict");
                const auto wordWidth = juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), word)) + Space::s;
                g.drawText (word, area.removeFromRight (wordWidth), juce::Justification::centredRight, false);
            }

            drawLine (g, area, colour, p.textMuted, line.change.name, line.change.what, line.change.kind == VersionChange::Kind::removed);
            return;
        }

        if (! isStaged && view.lines == Lines::clashes)
        {
            // A conflict, its letter C as VS Code marks a conflicting file.
            const auto& clash = view.clashes[(size_t) row];
            takeIconColumn (area, History::rowIconSize);
            drawLetter (g, area, "C", p.warning);
            drawLine (g, area, p.text, p.textMuted, clash.line + ": " + clash.what, describeValue (clash, clash.chosen));
            return;
        }

        // Its icon, its name and what changed, its actions under the pointer, and its letter, in its status's color.
        const auto& change = getLine (row);
        const auto colour = getChangeColour (getKind (change), theme);
        auto& icon = change.isSong ? view.songIcon : change.isMaster ? view.masterIcon : view.trackIcon;
        icon.draw (g, takeIconColumn (area, History::rowIconSize), p.textMuted);
        drawLetter (g, area, getLetter (change), colour);

        if (view.showsActions (getList(), row))
        {
            const auto drawAction = [&] (TintedIcon& action)
            {
                const auto spot = area.removeFromRight (History::actionWidth).toFloat();
                action.draw (g, spot.withSizeKeepingCentre (History::rowIconSize, History::rowIconSize), p.text);
            };

            if (isStaged)
            {
                drawAction (view.unstageIcon);
            }
            else
            {
                if (view.lines == Lines::changes && getComparedVersion (view.context).isEmpty())
                    drawAction (view.stageIcon);
                else
                    area.removeFromRight (History::actionWidth);

                drawAction (view.discardIcon);
            }
        }

        drawLine (g, area, colour, p.textMuted, change.name, change.what, change.kind == VersionChange::Kind::removed);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;

        if (! isStaged && view.lines == Lines::merge)
        {
            tickMergeLine (view.context, row, ! view.mergeLines[(size_t) row].ticked);
            return;
        }

        if (! isStaged && view.lines == Lines::clashes)
        {
            view.showClashMenu (row);
            return;
        }

        // A click selects what changed: its track, or the master; on one of its actions, runs it; the right button shows what
        // can be done with it.
        const auto change = getLine (row);
        const auto at = e.getEventRelativeTo (&getList()).getPosition();
        selectChange (view.context, change);

        if (! e.mods.isPopupMenu())
        {
            if (isStaged && view.getUnstagePosition (row).contains (at))
            {
                unstageChange (view.context, change);
                return;
            }

            if (! isStaged && view.getStagePosition (row).contains (at))
            {
                stageChange (view.context, change);
                return;
            }

            if (! isStaged && view.getPutBackPosition (row).contains (at))
            {
                view.commands.invoke ("version.putBack");
                return;
            }
        }

        if (change.track.isValid() && change.kind != VersionChange::Kind::removed)
            view.context.getSongSelection().selectOnly (change.track);

        if (e.mods.isPopupMenu() && isStaged)
            view.showMenu ({ "version.unstage" });
        else if (e.mods.isPopupMenu())
            view.showMenu ({ "version.stage", "version.putBack" });
    }

    void selectedRowsChanged (int row) override
    {
        // One line selected in the two lists.
        if (juce::isPositiveAndBelow (row, getNumRows()))
        {
            (isStaged ? view.changesList : view.stagedList).deselectAllRows();

            if (isStaged || view.lines == Lines::changes)
                selectChange (view.context, getLine (row));
        }

        getList().repaint(); // the actions on the row selected now
    }

    void returnKeyPressed (int row) override
    {
        if (! isStaged && view.lines == Lines::merge && juce::isPositiveAndBelow (row, (int) view.mergeLines.size()))
            tickMergeLine (view.context, row, ! view.mergeLines[(size_t) row].ticked);
        else if (! isStaged && view.lines == Lines::clashes)
            view.showClashMenu (row);
    }

    juce::String getNameForRow (int row) override { return isStaged ? view.describeStaged (row) : view.describeChange (row); }

    juce::String getTooltipForRow (int row) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return {};

        if (isStaged)
            return view.describeStaged (row) + ". Click to select it in the song; its minus unstages it.";

        if (view.lines == Lines::merge)
            return view.describeChange (row) + ". Click to take it, or to leave it out.";

        if (view.lines == Lines::clashes)
            return view.describeChange (row) + ". Both lines changed it: click to choose whose value the song has.";

        return view.describeChange (row) + ". Click to select it in the song; its plus stages it, its arrow discards it.";
    }

private:
    VersionsView& view;
    const bool isStaged;

    const VersionChange& getLine (int row) const { return isStaged ? view.staged[(size_t) row] : view.changes[(size_t) row]; }
    juce::ListBox& getList() const { return isStaged ? view.stagedList : view.changesList; }
    ListRowHover& getHover() const { return isStaged ? *view.stagedHover : *view.changesHover; }
};

//==============================================================================
// The graph: the uncommitted changes, the song as it is, then every branch's commits, newest first, each a dot on its
// branch's line, in its branch's column, and on a branch's last commit the branch's name; under a commit opened, what it
// changed, a row a line.
class VersionsView::HistoryModel : public juce::ListBoxModel
{
public:
    explicit HistoryModel (VersionsView& v)
        : view (v)
    {
    }

    int getNumRows() override { return view.getNumHistoryRows(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        const auto& p = getPalette (view);
        auto area = juce::Rectangle<int> (width, height);

        drawListRow (g, view, area.toFloat(), selected, view.historyHover != nullptr && row == view.historyHover->row);

        // From where a first level's chevron is, to inside the row's inset (VS Code's graph, SideBar).
        area = area.withTrimmedLeft (SideBar::rowChevron).withTrimmedRight (Space::s);
        const auto lanes = juce::jmax (1, (int) view.branches.size());
        const auto laneArea = area.removeFromLeft (lanes * History::laneWidth + Space::xs);
        const auto laneCentre = [&laneArea] (int lane)
        { return (float) laneArea.getX() + ((float) lane + 0.5f) * (float) History::laneWidth; };
        const auto middle = (float) height / 2.0f;

        // The lines through this row, under its dot.
        for (const auto& line : view.historyLines)
        {
            if (row < line.top || row > line.bottom || line.top == line.bottom)
                continue;

            const auto top = laneCentre (line.topLane), bottom = laneCentre (line.bottomLane);
            const auto down = line.turnsAtBottom ? top : bottom; // the column it goes down
            g.setColour (line.colour);

            if (row == line.top)
                g.drawLine (top, middle, down, (float) height, History::lineWidth);
            else if (row == line.bottom)
                g.drawLine (down, 0.0f, bottom, middle, History::lineWidth);
            else
                g.drawLine (down, 0.0f, down, (float) height, History::lineWidth);
        }

        const auto dotAt = [&] (const juce::String& branch)
        { return juce::Rectangle<float> (History::dotSize, History::dotSize).withCentre ({ laneCentre (view.getLane (branch)), middle }); };

        // A line under its commit, one level in from the commit's name: its icon, its name and what changed, its Restore under
        // the pointer, and its letter, as git shows a commit's files.
        if (const auto* change = view.getRowChange (row))
        {
            const auto theme = getThemeOf (view);
            const auto colour = getChangeColour (getKind (*change), theme);
            area.removeFromLeft (SideBar::chevronSize + SideBar::indent);
            auto& icon = change->isSong ? view.songIcon : change->isMaster ? view.masterIcon : view.trackIcon;
            icon.draw (g, takeIconColumn (area, History::rowIconSize), p.textMuted);
            drawLetter (g, area, getLetter (*change), colour);

            if (view.historyHover != nullptr && (row == view.historyHover->row || selected))
                view.discardIcon.draw (g,
                                       area.removeFromRight (History::actionWidth)
                                           .toFloat()
                                           .withSizeKeepingCentre (History::rowIconSize, History::rowIconSize),
                                       p.text);

            drawLine (g, area, colour, p.textMuted, change->name, change->what, change->kind == VersionChange::Kind::removed);
            return;
        }

        // Its play mark, while any commit has a bounce: what is heard in the accent's color.
        if (view.anyBounce)
        {
            const auto mark =
                area.removeFromRight (History::playWidth).toFloat().withSizeKeepingCentre (History::playSize, History::playSize);

            if (view.hasPlayMark (row))
            {
                const auto* version = view.getRowVersion (row);
                const auto heard = version == nullptr ? view.heardVersion.isEmpty() : version->id == view.heardVersion;
                juce::Path triangle;
                triangle.addTriangle (mark.getTopLeft(), mark.getBottomLeft(), { mark.getRight(), mark.getCentreY() });
                g.setColour (heard ? p.accent : p.textMuted);
                g.fillPath (triangle);
            }
        }

        if (row == 0)
        {
            g.setColour (p.textMuted);
            g.fillEllipse (dotAt (view.currentBranch).reduced (1.0f));
            g.setColour (p.text);
            g.drawEllipse (dotAt (view.currentBranch).reduced (0.5f), History::lineWidth / 2.0f);
            g.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
            g.drawText ("Uncommitted Changes", area, juce::Justification::centredLeft, true);
            return;
        }

        const auto* found = view.getRowVersion (row);

        if (found == nullptr)
            return;

        const auto& version = *found;
        g.setColour (view.getBranchColour (version.branch));
        g.fillEllipse (dotAt (version.branch));

        // Its chevron, as a tree's row has one: open while what it changed shows under it.
        (version.id == view.openedVersion ? view.openIcon : view.foldedIcon)
            .draw (g,
                   area.removeFromLeft (SideBar::chevronSize).toFloat().withSizeKeepingCentre (History::rowIconSize, History::rowIconSize),
                   p.textMuted);

        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        const auto when = version.id == view.bouncingVersion
                              ? "bouncing " + juce::String (juce::roundToInt (view.bounceProgress * 100.0f)) + "%"
                              : describeWhen (version.saved, juce::Time::getCurrentTime());
        g.drawText (when, area.removeFromRight (juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), when)) + Space::xs),
                    juce::Justification::centredRight, false);

        // The names of the branches whose last commit it is, as VS Code's graph names them: the current one's filled.
        for (const auto& branch : view.branches)
        {
            if (branch.head != version.id)
                continue;

            const auto isCurrent = branch.id == view.currentBranch;
            area.removeFromRight (drawBadge (g, area, branch.name,
                                             isCurrent ? branch.colour : branch.colour.withAlpha (History::branchBadgeAlpha),
                                             isCurrent ? p.background : branch.colour)
                                  + Space::xs);
        }

        // The commit checked out on its own, which the song is now: filled, as the current branch's name is.
        if (version.id == view.checkedOut)
            area.removeFromRight (drawBadge (g, area, "detached", view.getBranchColour (version.branch), p.background) + Space::xs);

        auto text = area;

        if (version.note.isNotEmpty())
        {
            g.setColour (p.textMuted);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText (version.note, text.removeFromBottom (height / 2 - 1), juce::Justification::centredLeft, true);
            text = text.withTrimmedTop (2);
        }

        g.setColour (version.hasOwnName ? p.text : p.textMuted);
        g.setFont (juce::FontOptions (TextSize::body));
        g.drawText (version.name, text, juce::Justification::centredLeft, true);
    }

    juce::String getNameForRow (int row) override { return view.describeHistoryRow (row); }

    juce::String getTooltipForRow (int row) override
    {
        const auto play = view.hasPlayMark (row) ? " Its play mark hears it." : "";

        if (row == 0)
            return "Uncommitted Changes: the song as it is, with its changes since the last commit. Right-click to create a branch "
                   "from it."
                   + juce::String (play);

        if (const auto* version = view.getLineVersion (row))
            return view.describeHistoryRow (row) + ". Its arrow restores it from \"" + version->name + "\"; right-click for more.";

        return view.describeHistoryRow (row) + ". Click to show what it changed; right-click for what can be done with it." + play;
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        selectedRowsChanged (row);
        const auto at = e.getEventRelativeTo (&view.historyList).getPosition();

        if (e.mods.isPopupMenu())
        {
            view.makeHistoryMenu (row).showMenuAsync (juce::PopupMenu::Options().withMousePosition());
            return;
        }

        // On its play mark: it is heard (the uncommitted changes': the song again).
        if (view.hasPlayMark (row) && view.getPlayMarkPosition (row).contains (at))
        {
            const auto* version = view.getRowVersion (row);
            hearVersion (view.context, version != nullptr ? version->id : juce::String());
            return;
        }

        // A line under a commit: its arrow restores it from that commit, as one undo step; a click selects its track.
        if (const auto* change = view.getRowChange (row))
        {
            if (view.getRestorePosition (row).contains (at))
            {
                view.commands.invoke ("version.restoreChange");
                return;
            }

            if (change->track.isValid() && change->kind != VersionChange::Kind::removed)
                view.context.getSongSelection().selectOnly (change->track);

            return;
        }

        // A commit opens, or folds.
        if (const auto* version = view.getRowVersion (row))
            view.openVersion (version->id);
    }

    // A double-click on a commit checks it out, as GitKraken's and Sourcetree's do.
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) override
    {
        if (view.getRowVersion (row) != nullptr && ! e.mods.isPopupMenu())
            view.commands.invoke ("version.checkout");
    }

    void returnKeyPressed (int row) override
    {
        if (const auto* version = view.getRowVersion (row))
            view.openVersion (version->id);
    }

    void selectedRowsChanged (int row) override
    {
        // The uncommitted changes, a commit, or a line under one, which the commands act on.
        if (const auto* change = view.getRowChange (row))
            selectVersionChange (view.context, view.getLineVersion (row)->id, *change);
        else if (const auto* version = view.getRowVersion (row))
            selectVersion (view.context, version->id);
        else
            selectVersion (view.context, {});

        view.historyList.repaint(); // the actions on the row selected now
    }

private:
    VersionsView& view;
};

//==============================================================================
// Local History: the song as it was at times while it differed from its last commit, newest first, each with why it was
// taken, the branch it was on and when; its Restore under the pointer or on the row selected.
class VersionsView::LocalHistoryModel : public juce::ListBoxModel
{
public:
    explicit LocalHistoryModel (VersionsView& v)
        : view (v)
    {
    }

    int getNumRows() override { return view.getNumSnapshots(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;

        const auto& p = getPalette (view);
        const auto& snapshot = view.snapshots[(size_t) row];
        drawListRow (g, view, juce::Rectangle<int> (width, height).toFloat(), selected,
                     view.localHover != nullptr && row == view.localHover->row);
        auto area = getRowContent (width, height);

        // Its clock, its time at its end, its Restore before it, and why it was taken with its branch.
        view.clockIcon.draw (g, takeIconColumn (area, History::rowIconSize), p.textMuted);
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        const auto when = describeWhen (juce::Time (snapshot.time), juce::Time::getCurrentTime());
        g.drawText (when, area.removeFromRight (juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), when)) + Space::xs),
                    juce::Justification::centredRight, false);

        if (view.localHover != nullptr && (row == view.localHover->row || selected))
            view.discardIcon.draw (
                g, area.removeFromRight (History::actionWidth).toFloat().withSizeKeepingCentre (History::rowIconSize, History::rowIconSize),
                p.text);

        drawLine (g, area, p.text, p.textMuted, snapshot.reason, snapshot.branch);
    }

    juce::String getNameForRow (int row) override { return view.describeSnapshot (row); }

    juce::String getTooltipForRow (int row) override
    {
        return view.describeSnapshot (row) + ". Its arrow restores the song to it, as one undo step; right-click for more.";
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        selectedRowsChanged (row);

        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            addCommandItems (menu, view.commands, { "version.restoreSnapshot" });
            menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
            return;
        }

        if (view.getSnapshotRestorePosition (row).contains (e.getEventRelativeTo (&view.localList).getPosition()))
            view.commands.invoke ("version.restoreSnapshot");
    }

    void selectedRowsChanged (int row) override
    {
        selectSnapshot (view.context, juce::isPositiveAndBelow (row, getNumRows()) ? view.snapshots[(size_t) row].time : 0);
        view.localList.repaint(); // the actions on the row selected now
    }

private:
    VersionsView& view;
};

//==============================================================================
VersionsView::VersionsView (CommandBridge& bridge, ProjectContext& projectContext, Settings& s)
    : commands (bridge),
      context (projectContext),
      settings (s),
      branchButton (std::make_unique<BranchButton>()),
      detachedBar (std::make_unique<DetachedBar> (*this)),
      messageField (std::make_unique<MessageField>()),
      stagedSection (std::make_unique<SectionHeader> ("Staged Changes", SectionHeader::Style::group)),
      changesSection (std::make_unique<SectionHeader> ("Changes", SectionHeader::Style::group)),
      historySection (std::make_unique<SectionHeader> ("Graph", SectionHeader::Style::pane)),
      localSection (std::make_unique<SectionHeader> ("Local History", SectionHeader::Style::pane)),
      stagedModel (std::make_unique<ChangesModel> (*this, true)),
      changesModel (std::make_unique<ChangesModel> (*this, false)),
      historyModel (std::make_unique<HistoryModel> (*this)),
      localModel (std::make_unique<LocalHistoryModel> (*this)),
      stagedList ("Staged Changes", stagedModel.get()),
      changesList ("Changes", changesModel.get()),
      historyList ("Graph", historyModel.get()),
      localList ("Local History", localModel.get())
{
    setTitle ("Source Control");

    // Its icon gives it the keys (and a screen reader its name), not the commit's message, so that Space plays on; Tab goes on
    // into its parts.
    setWantsKeyboardFocus (true);

    for (auto* label : { &emptyText, &roomText })
    {
        label->setFont (juce::FontOptions (TextSize::small));
        label->setJustificationType (juce::Justification::topLeft);
        setThemeColour (*label, juce::Label::textColourId, &Palette::textMuted);
    }

    roomText.setTitle ("Room the history takes");
    branchButton->onClick = [this] { commands.invoke ("version.pickBranch"); };
    moreButton.setQuiet (true);
    moreButton.onClick = [this] { makeMoreMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&moreButton)); };
    title.addAction (moreButton);

    // One message, as a commit's: its first line names the commit, the lines after it are its note.
    messageField->setTitle ("Commit message");
    messageField->setMultiLine (true, true);
    messageField->setReturnKeyStartsNewLine (true);
    messageField->setScrollbarsShown (true);
    messageField->setFont (juce::FontOptions (TextSize::body));
    messageField->addListener (this);
    messageField->onSave = [this]
    {
        takeMessage(); // as typed, the last key too
        commands.invoke ("version.save");
    };

    // Commit across the view in the accent color, its menu's part joined at its right end.
    saveButton.setAccented (true);
    saveButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    saveButton.onClick = [this]
    {
        takeMessage();
        commands.invoke ("version.save");
    };

    saveMenuButton.setAccented (true);
    saveMenuButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    saveMenuButton.onClick = [this] { makeSaveMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&saveMenuButton)); };

    // The groups' actions under the pointer, as VS Code's, from the right.
    stagedSection->addAction (Icon::minus, "Unstage All Changes", [this] { commands.invoke ("version.unstageAll"); });
    changesSection->addAction (Icon::plus, "Stage All Changes", [this] { commands.invoke ("version.stageAll"); });
    changesSection->addAction (Icon::undo, "Discard All Changes", [this] { commands.invoke ("version.discardAll"); });

    for (auto* section : { stagedSection.get(), changesSection.get(), historySection.get(), localSection.get() })
        section->onClick = [this] { resized(); };

    localSection->setToggleState (false, juce::dontSendNotification); // folded at first, its count on its badge

    doneButton.setButtonText ("Done");
    doneButton.onClick = [this] { commands.invoke ("version.showChanges"); };
    mergeButton.setButtonText ("Merge");
    mergeButton.onClick = [this] { commands.invoke ("version.finishMerge"); };
    cancelButton.setButtonText ("Abort");
    cancelButton.onClick = [this] { commands.invoke ("version.cancelMerge"); };

    for (auto* list : { &stagedList, &changesList, &historyList, &localList })
    {
        list->setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list->setTitle (list->getName());
    }

    stagedList.setRowHeight (History::changeRowHeight);
    changesList.setRowHeight (History::changeRowHeight);
    historyList.setRowHeight (History::versionRowHeight);
    localList.setRowHeight (History::changeRowHeight);

    const auto repaintRows = [] (juce::ListBox& list)
    {
        return [&list] (int previous, int row)
        {
            list.repaintRow (previous);
            list.repaintRow (row);
        };
    };

    stagedHover = std::make_unique<ListRowHover> (stagedList, repaintRows (stagedList));
    changesHover = std::make_unique<ListRowHover> (changesList, repaintRows (changesList));
    historyHover = std::make_unique<ListRowHover> (historyList, repaintRows (historyList));
    localHover = std::make_unique<ListRowHover> (localList, repaintRows (localList));

    for (auto* c : std::initializer_list<juce::Component*> { &title, branchButton.get(), messageField.get(), &saveButton, &saveMenuButton,
                                                             changesSection.get(), &changesList, historySection.get(), &historyList,
                                                             localSection.get(), &localList })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &emptyText, &doneButton, &mergeButton, &cancelButton, &roomText,
                                                             stagedSection.get(), &stagedList, detachedBar.get() })
        addChildComponent (c);

    refreshTooltips();
    context.addListener (this);
    refresh();
}

VersionsView::~VersionsView()
{
    stagedHover = nullptr;
    changesHover = nullptr;
    historyHover = nullptr;
    localHover = nullptr;
    context.removeListener (this);
}

juce::Button& VersionsView::getLocalHistorySection() noexcept
{
    return *localSection;
}

juce::String VersionsView::describeSnapshot (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) snapshots.size()))
        return {};

    const auto& snapshot = snapshots[(size_t) row];
    return snapshot.reason + ", " + describeWhen (juce::Time (snapshot.time), juce::Time::getCurrentTime())
           + (snapshot.branch.isNotEmpty() ? ", on " + snapshot.branch : juce::String());
}

juce::Rectangle<int> VersionsView::getSnapshotRestorePosition (int row) const
{
    // Before the time at its end (LocalHistoryModel::paintListBoxItem()), under the pointer or on the row selected.
    if (! juce::isPositiveAndBelow (row, (int) snapshots.size()) || localHover == nullptr
        || (row != localHover->row && ! localList.isRowSelected (row)))
        return {};

    const auto when = describeWhen (juce::Time (snapshots[(size_t) row].time), juce::Time::getCurrentTime());
    const auto whenWidth =
        juce::roundToInt (juce::TextLayout::getStringWidth (juce::Font (juce::FontOptions (TextSize::small)), when)) + Space::xs;
    return localList.getRowPosition (row, true).withTrimmedRight (Space::s + whenWidth).removeFromRight (History::actionWidth);
}

juce::TextEditor& VersionsView::getMessageField() noexcept
{
    return *messageField;
}

juce::Button& VersionsView::getBranchButton() noexcept
{
    return *branchButton;
}

bool VersionsView::showsCheckout() const noexcept
{
    return detachedBar->isVisible();
}

juce::Button& VersionsView::getBackButton() noexcept
{
    return detachedBar->backButton;
}

juce::Button& VersionsView::getBranchHereButton() noexcept
{
    return detachedBar->branchButton;
}

juce::Button& VersionsView::getChangesSection() noexcept
{
    return *changesSection;
}

juce::Button& VersionsView::getStagedSection() noexcept
{
    return *stagedSection;
}

juce::Button& VersionsView::getHistorySection() noexcept
{
    return *historySection;
}

juce::String VersionsView::getChangesHeading() const
{
    return changesSection->getText();
}

void VersionsView::refreshTooltips()
{
    saveButton.setTooltip (withShortcut (commands,
                                         "Saves the project and commits the staged changes, or every change while none is staged, "
                                         "named by the message's first line, or by what changed",
                                         "version.save"));
    saveMenuButton.setTooltip ("Commit's choices: whether each commit is bounced as it is made");
    messageField->setTooltip ("The commit's message: its first line is its name, the lines after it a note. Left empty, the commit is "
                              "named by what changed. Ctrl+Enter commits");
    changesList.setTooltip ("What changed in the song since the last commit, or since it was staged, by track. U untracked, M "
                            "modified, D deleted");
    stagedList.setTooltip ("What the next commit records, by track. A added, M modified, D deleted");
    doneButton.setTooltip (withShortcut (commands, "Lists the changes since the last commit again", "version.showChanges"));
    mergeButton.setTooltip (withShortcut (commands, "Takes the lines ticked into the song, as one undo step", "version.finishMerge"));
    cancelButton.setTooltip (withShortcut (commands,
                                           "Aborts the merge: closes its list, taking nothing, or, once taken, makes the song what it "
                                           "was before it, as one undo step",
                                           "version.cancelMerge"));
    branchButton->setTooltip (
        withShortcut (commands, "The branch the song is on: click to check out another, or to create one", "version.pickBranch"));
    moreButton.setTooltip ("More Actions: commit, changes, branches, DAWproject, clean up");
    historyList.setTooltip ("Every commit, newest first, each on its branch's line: click one to show what it changed");
    localSection->setTooltip ("The song as it was while it differed from its last commit, kept without asking: as the project opened, "
                              "every few minutes while it changed, and before what made it another");
    localList.setTooltip ("Local History, newest first: each one's arrow restores the song to it, as one undo step");
    roomText.setTooltip ("What the history takes on disk: nearly all of it is bounces, each deleted on its own (Delete Bounce)");
}

juce::PopupMenu VersionsView::makeSaveMenu()
{
    juce::PopupMenu menu;
    addCommandItems (menu, commands, { "version.save" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "version.bounceEach" });
    return menu;
}

juce::PopupMenu VersionsView::makeMoreMenu()
{
    // As VS Code's Source Control menu: the commands by what they act on.
    juce::PopupMenu menu, commitMenu, changesMenu, branchMenu;
    addCommandItems (commitMenu, commands, { "version.save", "version.undoCommit" });
    commitMenu.addSeparator();
    addCommandItems (commitMenu, commands, { "version.bounceEach" });
    addCommandItems (changesMenu, commands, { "version.stageAll", "version.unstageAll", "version.discardAll" });
    addCommandItems (branchMenu, commands, { "version.pickBranch", "version.mergeBranch", "version.renameBranch", "version.deleteBranch" });
    menu.addSubMenu ("Commit", commitMenu);
    menu.addSubMenu ("Changes", changesMenu);
    menu.addSubMenu ("Branch", branchMenu);
    menu.addSeparator();
    addCommandItems (menu, commands, { "version.exportDawProject", "project.importDawProject" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "project.cleanUpAudio", "version.verify", "version.recover" });
    return menu;
}

bool VersionsView::hasPlayMark (int row) const
{
    if (! anyBounce)
        return false;

    const auto* version = getRowVersion (row);
    return row == 0 || (version != nullptr && version->hasBounce);
}

const VersionInfo* VersionsView::getRowVersion (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) historyRows.size()))
        return nullptr;

    const auto& place = historyRows[(size_t) row];
    return place.change < 0 && juce::isPositiveAndBelow (place.version, (int) versions.size()) ? &versions[(size_t) place.version]
                                                                                               : nullptr;
}

const VersionChange* VersionsView::getRowChange (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) historyRows.size()))
        return nullptr;

    const auto& place = historyRows[(size_t) row];
    return juce::isPositiveAndBelow (place.change, (int) openedChanges.size()) ? &openedChanges[(size_t) place.change] : nullptr;
}

const VersionInfo* VersionsView::getLineVersion (int row) const
{
    if (getRowChange (row) == nullptr)
        return nullptr;

    const auto& place = historyRows[(size_t) row];
    return juce::isPositiveAndBelow (place.version, (int) versions.size()) ? &versions[(size_t) place.version] : nullptr;
}

int VersionsView::getHistoryRow (const juce::String& versionID) const
{
    if (versionID.isEmpty())
        return 0;

    for (int row = 0; row < (int) historyRows.size(); ++row)
        if (const auto* version = getRowVersion (row); version != nullptr && version->id == versionID)
            return row;

    return -1;
}

void VersionsView::refreshHistoryRows()
{
    // The commit opened, as long as it is there: what it changed under it.
    const auto opened = std::find_if (versions.begin(), versions.end(), [this] (const VersionInfo& v) { return v.id == openedVersion; });

    if (opened == versions.end())
    {
        openedVersion.clear();
        openedChanges.clear();
    }
    else
    {
        openedChanges = getVersionChanges (context, openedVersion);
    }

    historyRows.clear();
    historyRows.push_back ({});

    for (int i = 0; i < (int) versions.size(); ++i)
    {
        historyRows.push_back ({ i, -1 });

        if (versions[(size_t) i].id == openedVersion)
            for (int c = 0; c < (int) openedChanges.size(); ++c)
                historyRows.push_back ({ i, c });
    }
}

void VersionsView::selectHistoryRow()
{
    // What the model selects, wherever its row went: a line under a commit, the commit, or the uncommitted changes.
    const auto selected = getSelectedVersion (context);
    const auto line = getSelectedVersionChange (context);
    auto row = getHistoryRow (selected);

    if (line.has_value() && row >= 0)
    {
        for (int r = row + 1; r < (int) historyRows.size() && getRowChange (r) != nullptr; ++r)
        {
            const auto* change = getRowChange (r);

            if (change->track == line->track && change->isSong == line->isSong && change->isMaster == line->isMaster)
                row = r;
        }
    }

    if (row >= 0 && historyList.getSelectedRow() != row && historyList.getNumSelectedRows() > 0)
        historyList.selectRow (row, true, true);
}

void VersionsView::openVersion (const juce::String& versionID)
{
    // Another click folds it; one commit opens at a time.
    openedVersion = openedVersion == versionID ? juce::String() : versionID;
    refreshHistoryRows();
    refreshHistoryLines (getCurrentBranch (context).has_value() ? getCurrentBranch (context)->head : juce::String());
    historyList.updateContent();

    if (const auto row = getHistoryRow (versionID); row >= 0)
        historyList.selectRow (row, true, true);

    historyList.repaint();
}

juce::Rectangle<int> VersionsView::getRestorePosition (int row) const
{
    // Before the letter of a line under its commit, inside the row's margin (HistoryModel::paintListBoxItem()).
    if (getRowChange (row) == nullptr || historyHover == nullptr || (row != historyHover->row && ! historyList.isRowSelected (row)))
        return {};

    return historyList.getRowPosition (row, true).withTrimmedRight (Space::s + History::letterWidth).removeFromRight (History::actionWidth);
}

juce::PopupMenu VersionsView::makeHistoryMenu (int row)
{
    // As VS Code's graph's menu, grouped: hearing it; going back to it; branching and merging; its message; its bounce; and
    // exporting it. A line under a commit: restoring it.
    juce::PopupMenu menu;
    const auto group = [this, &menu] (std::initializer_list<const char*> ids)
    {
        if (menu.getNumItems() > 0)
            menu.addSeparator();

        addCommandItems (menu, commands, ids);
    };

    if (getRowChange (row) != nullptr)
    {
        group ({ "version.restoreChange" });
        return menu;
    }

    if (const auto* version = getRowVersion (row))
    {
        const auto last = getLastVersion (context);
        group ({ "version.hear" });
        group ({ "version.checkout", "version.bringBack", "version.takeFrom" });
        group ({ "version.newBranch", "version.merge" });

        if (last.has_value() && last->id == version->id)
            group ({ "version.editMessage", "version.undoCommit" });
        else
            group ({ "version.editMessage" });

        group ({ "version.bounce", "version.deleteBounce" });
        group ({ "version.exportDawProject" });
        return menu;
    }

    group ({ "version.hear" });
    group ({ "version.newBranch" });

    if (checkedOut.isNotEmpty())
        group ({ "version.backToBranch" });

    group ({ "version.exportDawProject" });
    return menu;
}

juce::Rectangle<int> VersionsView::getPlayMarkPosition (int row) const
{
    // The last column of the row, inside its margin (HistoryModel::paintListBoxItem()).
    const auto bounds = historyList.getRowPosition (row, true);
    return bounds.withTrimmedRight (Space::s).removeFromRight (History::playWidth);
}

bool VersionsView::showsActions (const juce::ListBox& list, int row) const
{
    const auto& hover = &list == &stagedList ? *stagedHover : *changesHover;
    const auto rows = &list == &stagedList ? (int) staged.size() : (int) changes.size();
    return juce::isPositiveAndBelow (row, rows) && (&list == &stagedList || lines == Lines::changes)
           && (row == hover.row || list.isRowSelected (row));
}

juce::Rectangle<int> VersionsView::getActionPosition (const juce::ListBox& list, int row, int fromRight) const
{
    // Before the letter, inside the row's margin (ChangesModel::paintListBoxItem()): the first action nearest the letter.
    if (! showsActions (list, row))
        return {};

    auto bounds = list.getRowPosition (row, true).withTrimmedRight (Space::s + History::letterWidth);
    bounds.removeFromRight (fromRight * History::actionWidth);
    return bounds.removeFromRight (History::actionWidth);
}

juce::Rectangle<int> VersionsView::getStagePosition (int row) const
{
    return getComparedVersion (context).isEmpty() ? getActionPosition (changesList, row, 0) : juce::Rectangle<int>();
}

juce::Rectangle<int> VersionsView::getPutBackPosition (int row) const
{
    return getActionPosition (changesList, row, 1);
}

juce::Rectangle<int> VersionsView::getUnstagePosition (int row) const
{
    return getActionPosition (stagedList, row, 0);
}

int VersionsView::getLane (const juce::String& branchID) const
{
    for (size_t i = 0; i < branches.size(); ++i)
        if (branches[i].id == branchID)
            return (int) i;

    return 0;
}

juce::Colour VersionsView::getBranchColour (const juce::String& id) const
{
    for (const auto& branch : branches)
        if (branch.id == id)
            return branch.colour;

    return getPalette (*this).textMuted;
}

juce::String VersionsView::getBranchName (const juce::String& id) const
{
    for (const auto& branch : branches)
        if (branch.id == id)
            return branch.name;

    return {};
}

int VersionsView::getNumChanges() const noexcept
{
    return lines == Lines::merge ? (int) mergeLines.size() : lines == Lines::clashes ? (int) clashes.size() : (int) changes.size();
}

juce::String VersionsView::describeChange (int row) const
{
    if (! juce::isPositiveAndBelow (row, getNumChanges()))
        return {};

    if (lines == Lines::merge)
    {
        const auto& line = mergeLines[(size_t) row];
        return (line.ticked ? "Taken: " : "Left out: ") + describeChangeLine (line.change) + (line.clashes ? ", conflict" : "");
    }

    if (lines == Lines::clashes)
    {
        const auto& clash = clashes[(size_t) row];
        return clash.line + ": " + clash.what + ", Conflict, " + describeValue (clash, clash.chosen);
    }

    return describeChangeLine (changes[(size_t) row]);
}

juce::String VersionsView::getChangeLetter (int row) const
{
    if (lines == Lines::merge && juce::isPositiveAndBelow (row, (int) mergeLines.size()))
        return getLetter (mergeLines[(size_t) row].change);

    if (lines == Lines::changes && juce::isPositiveAndBelow (row, (int) changes.size()))
        return getLetter (changes[(size_t) row]);

    return lines == Lines::clashes && juce::isPositiveAndBelow (row, (int) clashes.size()) ? juce::String ("C") : juce::String();
}

juce::String VersionsView::describeStaged (int row) const
{
    return juce::isPositiveAndBelow (row, (int) staged.size()) ? describeChangeLine (staged[(size_t) row]) : juce::String();
}

juce::String VersionsView::getStagedLetter (int row) const
{
    return juce::isPositiveAndBelow (row, (int) staged.size()) ? getLetter (staged[(size_t) row]) : juce::String();
}

juce::String VersionsView::describeHistoryRow (int row) const
{
    if (row == 0)
        return heardVersion.isEmpty() && anyBounce ? "Uncommitted Changes, heard" : "Uncommitted Changes";

    // A line under its commit, as git names a commit's file's status: "Bass, Modified: volume".
    if (const auto* change = getRowChange (row))
    {
        const juce::String status = change->kind == VersionChange::Kind::added     ? "Added"
                                    : change->kind == VersionChange::Kind::removed ? "Deleted"
                                                                                   : "Modified";
        return change->name + ", " + status + (change->what.isEmpty() ? juce::String() : ": " + change->what);
    }

    const auto* found = getRowVersion (row);

    if (found == nullptr)
        return {};

    const auto& version = *found;
    juce::String text = version.name + ", " + describeWhen (version.saved, juce::Time::getCurrentTime());

    if (const auto branch = getBranchName (version.branch); branch.isNotEmpty())
        text << ", on " << branch;

    if (version.hasBounce)
        text << (version.id == heardVersion ? ", heard" : ", with a bounce");
    else if (version.id == bouncingVersion)
        text << ", bouncing";

    if (version.note.isNotEmpty())
        text << ". " << version.note;

    // What it changed, where its name doesn't say it; and whether what it changed shows under it.
    if (version.hasOwnName && version.summary.isNotEmpty() && version.summary != version.name)
        text << ". " << version.summary;

    if (version.id == openedVersion)
        text << ", open";

    return text;
}

void VersionsView::showMenu (std::initializer_list<const char*> commandIDs)
{
    juce::PopupMenu menu;
    addCommandItems (menu, commands, commandIDs);
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void VersionsView::showClashMenu (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) clashes.size()))
        return;

    // Its three values, as a merge editor offers them, the one the song has ticked: a choice is heard at once.
    juce::PopupMenu menu;
    const auto& clash = clashes[(size_t) index];

    for (int value : { 1, 2, 0 })
        menu.addItem (getClashChoiceName (value) + " (" + clash.values[value] + ")", true, clash.chosen == value,
                      [this, index, value] { chooseClash (context, index, value); });

    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void VersionsView::refresh()
{
    const auto keeps = keepsVersions (context);
    const auto reading = isReadingVersions (context);
    staged = getStagedChanges (context);
    changes = getListedChanges (context);
    mergeLines = getMergeLines (context);
    clashes = getClashes (context);
    versions = getVersions (context);
    branches = getBranches (context);
    snapshots = getSnapshots (context);
    heardVersion = getHeardVersion (context);
    anyBounce = std::any_of (versions.begin(), versions.end(), [] (const VersionInfo& version) { return version.hasBounce; });
    bouncingVersion.clear();

    for (const auto& version : versions)
    {
        if (const auto progress = getBounceProgress (context, version.id))
        {
            bouncingVersion = version.id;
            bounceProgress = *progress;
        }
    }

    // Bounce Each Commit, as the settings keep it, for the song open now; and the room the history takes, or why an old
    // commit's bounce failed, until the next one.
    if (const auto bounces = settings.get (Settings::versionBounce); getBounceWithVersion (context) != bounces)
        setBounceWithVersion (context, bounces);

    // What needs saying first: a bounce or a restore that failed in part, what Verify History found, a line that went its
    // own way on another computer; else the room the history takes.
    const auto room = getVersionsRoom (context);
    juce::String notice;

    for (const auto& text :
         { getBounceProblem (context), getRestoreProblem (context), describeHistoryCheck (context), describeDivergence (context) })
        if (notice.isEmpty())
            notice = text;

    roomText.setText (notice.isNotEmpty() ? notice
                      : versions.empty()  ? juce::String()
                                          : "Commits take " + juce::File::descriptionOfSizeInBytes (room.songs) + ", bounces "
                                                + juce::File::descriptionOfSizeInBytes (room.bounces),
                      juce::dontSendNotification);
    roomText.setVisible (roomText.getText().isNotEmpty());

    const auto branch = getCurrentBranch (context);
    currentBranch = branch.has_value() ? branch->id : juce::String();
    const auto detached = branch.has_value() && branch->detached;
    checkedOut = detached ? branch->head : juce::String();
    refreshHistoryRows();
    refreshHistoryLines (branch.has_value() ? branch->head : juce::String());
    branchButton->setBranch (! branch.has_value() ? juce::String()
                             : detached           ? branch->name + " (detached)"
                                                  : branch->name,
                             branch.has_value() ? branch->colour : getPalette (*this).textMuted);
    branchButton->setVisible (branch.has_value());

    // A commit checked out on its own: the bar that says so, with the ways on.
    if (detached)
        detachedBar->show (*branch, describeBackToBranch (context));

    if (detachedBar->isVisible() != detached)
    {
        detachedBar->setVisible (detached);
        resized();
    }

    // A merge's list while it shows; its conflicts while it waits for its commit; otherwise the changes.
    lines = ! mergeLines.empty() ? Lines::merge : ! clashes.empty() ? Lines::clashes : Lines::changes;
    const auto last = getLastVersion (context);
    const auto compared = getComparedVersion (context);
    const auto merged = describeMerge (context);
    const auto branchName = branch.has_value() ? branch->name : juce::String();
    juce::String comparedName;

    for (const auto& version : versions)
        if (version.id == compared)
            comparedName = version.name;

    if (lines == Lines::merge)
        changesSection->set ("Merge", "\"" + merged + "\" into " + branchName, getNumChanges());
    else if (lines == Lines::clashes)
        changesSection->set ("Merge Changes", {}, getNumChanges());
    else if (compared.isNotEmpty())
        changesSection->set ("Compare with", "\"" + comparedName + "\"", getNumChanges());
    else
        changesSection->set ("Changes", {}, getNumChanges());

    stagedSection->set ("Staged Changes", {}, (int) staged.size());
    stagedSection->setVisible (! staged.empty() && lines == Lines::changes && compared.isEmpty());
    stagedList.setVisible (stagedSection->isVisible());
    historySection->set ("Graph", {}, 0);
    localSection->set ("Local History", {}, (int) snapshots.size());
    localSection->setVisible (keeps);
    changesSection->setActionsAllowed (lines == Lines::changes && compared.isEmpty());
    doneButton.setVisible (lines == Lines::changes && compared.isNotEmpty());
    mergeButton.setVisible (lines == Lines::merge);
    cancelButton.setVisible (lines == Lines::merge || isMerging (context)); // a merge waiting for its commit can be aborted too

    // What there is to say while there is nothing to list, and while a merge waits for its commit.
    juce::String said = ! keeps   ? juce::String ("Source control starts once the project has a file.")
                        : reading ? juce::String ("Reading the commits...")
                        : ! last  ? juce::String ("No commits yet.")
                        : getNumChanges() == 0 && compared.isNotEmpty() ? juce::String ("The song is as that commit was.")
                        : getNumChanges() == 0 && staged.empty()        ? juce::String ("No changes since the last commit.")
                                                                        : juce::String();

    if (isMerging (context))
    {
        said = "Merging \"" + merged + "\": commit to finish." + (said.isNotEmpty() ? " " + said : juce::String());

        for (const auto& note : getMergeNotes (context))
            said << "\n" << note;
    }

    emptyText.setText (said, juce::dontSendNotification);
    emptyText.setVisible (said.isNotEmpty());

    // The draft the model holds, as one message, when it changed there (empty again once a commit was made with it, or
    // another project's): what is being typed stays as it is.
    if (const auto draft = getDraftMessage(); draft != takenMessage)
    {
        takenMessage = draft;
        messageField->setText (draft, false);
    }

    // While nothing is typed, the name the commit would get, as GitHub Desktop shows it ("Update Bass"), with the key that
    // commits, as VS Code's field says it.
    const auto onBranch = branch.has_value() ? " on '" + branch->name + "'" : juce::String();
    const auto commitKey =
        formatKey (juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::commandModifier, 0).getTextDescription())
            .replace ("Return", "Enter");
    const auto next = describeNextCommit (context);
    messageField->setTextToShowWhenEmpty (detached ? juce::String ("Create a branch here to commit")
                                                   : (next.isNotEmpty() ? next : juce::String ("Message")) + " (" + commitKey + " to commit"
                                                         + onBranch + ")",
                                          getPalette (*this).textMuted);
    messageField->setEnabled (keeps);

    saveButton.setEnabled (canSaveVersion (context));
    saveMenuButton.setEnabled (keeps);
    saveButton.setButtonText (isSavingVersion (context)        ? "Committing..."
                              : getBounceWithVersion (context) ? "Commit and Bounce"
                                                               : "Commit");

    stagedList.updateContent();
    changesList.updateContent();
    historyList.updateContent();
    localList.updateContent();
    selectHistoryRow();

    // The snapshot selected, wherever its row went as new ones came.
    if (localList.getNumSelectedRows() > 0)
        for (int row = 0; row < (int) snapshots.size(); ++row)
            if (snapshots[(size_t) row].time == getSelectedSnapshot (context) && localList.getSelectedRow() != row)
                localList.selectRow (row, true, true);

    stagedList.repaint();
    changesList.repaint();
    historyList.repaint();
    localList.repaint();
    resized();
}

void VersionsView::refreshHistoryLines (const juce::String& nowFollows)
{
    // Each commit's lines to the commits it follows, and the uncommitted changes' to the current branch's last commit (or, on a
    // branch without commits yet, the one it was made from).
    // By their rows, which the lines under the commit opened push down.
    std::map<juce::String, std::pair<int, juce::String>> rows; // a commit's row and branch

    for (int row = 0; row < (int) historyRows.size(); ++row)
        if (const auto* version = getRowVersion (row))
            rows[version->id] = { row, version->branch };

    historyLines.clear();

    const auto addLine = [this, &rows] (int row, const juce::String& branch, const juce::String& parent, bool first)
    {
        const auto found = rows.find (parent);

        if (found == rows.end())
            return;

        const auto& [parentRow, parentBranch] = found->second;
        historyLines.push_back (
            { row, parentRow, getLane (branch), getLane (parentBranch), first, getBranchColour (first ? branch : parentBranch) });
    };

    addLine (0, currentBranch, nowFollows, true);

    for (const auto& [id, place] : rows)
        if (const auto* version = getRowVersion (place.first))
            for (int p = 0; p < version->parents.size(); ++p)
                addLine (place.first, version->branch, version->parents[p], p == 0);
}

void VersionsView::takeMessage()
{
    const auto text = messageField->getText();
    setVersionDraft (context, text.upToFirstOccurrenceOf ("\n", false, false).trim(),
                     text.fromFirstOccurrenceOf ("\n", false, false).trim());
    takenMessage = getDraftMessage();
}

juce::String VersionsView::getDraftMessage() const
{
    const auto note = getVersionDraftNote (context);
    return getVersionDraftName (context) + (note.isNotEmpty() ? "\n" + note : juce::String());
}

void VersionsView::textEditorTextChanged (juce::TextEditor&)
{
    takeMessage();
    resized(); // the message grows with its lines
}

void VersionsView::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void VersionsView::resized()
{
    // As VS Code's side bar lays out its Source Control (SideBar): the lists as wide as the panel, their rows inset; the fields
    // and buttons in rows of their own, from the panel's sides.
    auto area = getLocalBounds();
    const auto inField = [] (juce::Rectangle<int> row, int height)
    {
        const auto field = row.withTrimmedLeft (SideBar::fieldLeft).withTrimmedRight (SideBar::fieldRight);
        return field.withSizeKeepingCentre (field.getWidth(), height);
    };

    // The title and the view's menu; then the branch, on a row of its own, as the panel is narrow.
    title.setBounds (area.removeFromTop (SideBar::titleHeight));

    if (branchButton->isVisible())
        branchButton->setBounds (area.removeFromTop (ListRow::height).reduced ((int) ListRow::inset, 0));

    // A commit checked out: the bar that says so, in the fields' width.
    if (detachedBar->isVisible())
    {
        const auto width = area.getWidth() - SideBar::fieldLeft - SideBar::fieldRight;
        detachedBar->setBounds (area.removeFromTop (detachedBar->getPreferredHeight (width))
                                    .withTrimmedLeft (SideBar::fieldLeft)
                                    .withTrimmedRight (SideBar::fieldRight));
        area.removeFromTop (Space::xs);
    }

    // The message, as tall as its lines as they wrap, up to a few: one line as tall as VS Code's, in its row.
    const auto lineHeight = juce::roundToInt (messageField->getFont().getHeight() * 1.2f);
    const auto fieldWidth = area.getWidth() - SideBar::fieldLeft - SideBar::fieldRight;
    juce::AttributedString text (messageField->getText().isEmpty() ? juce::String ("M") : messageField->getText());
    text.setFont (messageField->getFont());
    juce::TextLayout layout;
    layout.createLayout (text, (float) juce::jmax (1, fieldWidth - (SideBar::controlHeight - lineHeight)));
    const auto messageLines = juce::jlimit (1, History::messageLines, layout.getNumLines());
    const auto messageHeight = SideBar::controlHeight + lineHeight * (messageLines - 1);
    messageField->setBounds (
        inField (area.removeFromTop (messageHeight + SideBar::inputRowHeight - SideBar::controlHeight), messageHeight));

    auto saveRow = inField (area.removeFromTop (SideBar::buttonRowHeight), SideBar::controlHeight);
    saveMenuButton.setBounds (saveRow.removeFromRight (History::saveMenuWidth));
    saveRow.removeFromRight (1); // a hairline between the two parts
    saveButton.setBounds (saveRow);

    // The room the history takes, at the foot of the view.
    if (roomText.isVisible())
        roomText.setBounds (
            area.removeFromBottom (controlHeight).withTrimmedLeft (SideBar::fieldLeft - roomText.getBorderSize().getLeft()));

    // The staged changes and the changes, as many rows as they have while open (together up to half of what is left, the
    // staged ones up to half of that), then the graph.
    auto listsRoom = area.getHeight() / 2;

    if (stagedSection->isVisible())
    {
        stagedSection->setBounds (area.removeFromTop (stagedSection->getPreferredHeight()));
        const auto rows = stagedSection->getToggleState() ? (int) staged.size() : 0;
        const auto height = juce::jmin (listsRoom / 2, rows * History::changeRowHeight);
        stagedList.setBounds (rows == 0 ? juce::Rectangle<int>() : area.removeFromTop (height));
        listsRoom -= height;
    }

    changesSection->setBounds (area.removeFromTop (changesSection->getPreferredHeight()));
    const auto changesOpen = changesSection->getToggleState();

    if (doneButton.isVisible() && changesOpen)
    {
        auto buttons = inField (area.removeFromTop (SideBar::buttonRowHeight), SideBar::controlHeight);
        doneButton.setBounds (buttons.removeFromRight (juce::jmin (buttons.getWidth(), History::buttonWidth)));
    }

    if (cancelButton.isVisible() && changesOpen)
    {
        // Merge and Abort while a merge's lines show; Abort alone while it waits for its commit.
        auto buttons = inField (area.removeFromTop (SideBar::buttonRowHeight), SideBar::controlHeight);
        cancelButton.setBounds (buttons.removeFromRight (juce::jmin (buttons.getWidth() / 2, History::buttonWidth)));
        buttons.removeFromRight (Space::xs);

        if (mergeButton.isVisible())
            mergeButton.setBounds (buttons.removeFromRight (juce::jmin (buttons.getWidth(), History::buttonWidth)));
    }

    for (auto* button : { &doneButton, &mergeButton, &cancelButton })
        if (! changesOpen)
            button->setBounds ({});

    // A note in the group, where its rows' names would be.
    if (emptyText.isVisible() && changesOpen)
        emptyText.setBounds (area.removeFromTop (History::noteHeight + (isMerging (context) ? History::noteHeight : 0))
                                 .withTrimmedLeft (History::rowText - emptyText.getBorderSize().getLeft())
                                 .withTrimmedRight (SideBar::fieldRight));
    else
        emptyText.setBounds ({});

    const auto rows = changesOpen ? getNumChanges() : 0;
    const auto listHeight = juce::jmin (juce::jmax (listsRoom, History::changeRowHeight), rows * History::changeRowHeight);
    changesList.setBounds (rows == 0 ? juce::Rectangle<int>() : area.removeFromTop (listHeight));

    // Local History at the foot, its rows (while open) up to a third of what is left, then the graph above it.
    if (localSection->isVisible())
    {
        const auto localRows = localSection->getToggleState() ? (int) snapshots.size() : 0;
        const auto localHeight =
            localRows == 0 ? 0
                           : juce::jmin (juce::jmax (History::changeRowHeight, area.getHeight() / 3), localRows * History::changeRowHeight);
        localList.setBounds (localRows == 0 ? juce::Rectangle<int>() : area.removeFromBottom (localHeight));
        localSection->setBounds (area.removeFromBottom (localSection->getPreferredHeight()));
    }
    else
    {
        localSection->setBounds ({});
        localList.setBounds ({});
    }

    historySection->setBounds (area.removeFromTop (historySection->getPreferredHeight()));
    historyList.setBounds (historySection->getToggleState() ? area : juce::Rectangle<int>());
}
} // namespace phazite::ui
