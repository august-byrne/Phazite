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
        g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
        g.drawText (letter, area.removeFromRight (History::letterWidth), juce::Justification::centred, false);
    }

    // A badge's width, for a count or a branch's name, with the font set as drawBadge() sets it.
    int getBadgeWidth (const juce::String& text)
    {
        const auto font = juce::Font (juce::FontOptions (TextSize::badge, juce::Font::bold));
        return juce::roundToInt (juce::jmax (History::badgeHeight, juce::TextLayout::getStringWidth (font, text) + History::badgeHeight));
    }

    // A pill with a text in it, as VS Code counts a section's rows and names a branch on its last commit: its width.
    int drawBadge (juce::Graphics& g, juce::Rectangle<int> rightOf, const juce::String& text, juce::Colour fill, juce::Colour textColour)
    {
        // The font set, then measured, as the headings are: measured first, a bold one came out in a serif face.
        g.setFont (juce::FontOptions (TextSize::badge, juce::Font::bold));
        const auto width = juce::roundToInt (
            juce::jmax (History::badgeHeight, juce::TextLayout::getStringWidth (g.getCurrentFont(), text) + History::badgeHeight));
        const auto pill = rightOf.removeFromRight (width).toFloat().withSizeKeepingCentre ((float) width, History::badgeHeight);
        g.setColour (fill);
        g.fillRoundedRectangle (pill, History::badgeHeight / 2.0f);
        g.setColour (textColour);
        g.drawText (text, pill, juce::Justification::centred, false);
        return width;
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
            g.setColour (p.text.withAlpha (History::hoverWash));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);
        }

        auto area = getLocalBounds().reduced (Space::xs, 0);
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
// A section's heading, which folds it, as VS Code's: its chevron, its name and what it is about, its count's badge, and,
// under the pointer, its actions, each a command's icon.
class VersionsView::Section : public juce::Button
{
public:
    explicit Section (const juce::String& name)
        : juce::Button (name)
    {
        setTitle (name);
        setClickingTogglesState (true);
        setToggleable (true);
        setToggleState (true, juce::dontSendNotification); // open
        addMouseListener (&hover, false);
    }

    ~Section() override
    {
        removeMouseListener (&hover);

        for (auto& action : actions)
            action->removeMouseListener (&hover);
    }

    void set (const juce::String& newHeading, const juce::String& newDetail, int newCount)
    {
        if (heading == newHeading && detail == newDetail && count == newCount)
            return;

        heading = newHeading;
        detail = newDetail;
        count = newCount;
        setDescription (count > 0 ? juce::String (count) : juce::String());
        resized();
        repaint();
    }

    void addAction (Icon icon, const juce::String& name, std::function<void()> run)
    {
        auto action = std::make_unique<IconButton> (name, icon);
        action->setQuiet (true);
        action->setTooltip (name);
        action->onClick = std::move (run);
        action->addMouseListener (&hover, false);
        addChildComponent (*action);
        actions.push_back (std::move (action));
    }

    juce::String getText() const { return detail.isEmpty() ? heading : heading + " " + detail; }

    // Whether its actions show under the pointer: not while what it lists is a merge's or a comparison's.
    void setActionsAllowed (bool allowed)
    {
        actionsAllowed = allowed;
        showActions (isMouseOver (true));
    }

    void resized() override
    {
        // From the right: the count's badge, then the actions.
        auto area = getLocalBounds();
        area.removeFromRight (count > 0 ? getBadgeWidth (juce::String (count)) + Space::xs : 0);

        for (auto& action : actions)
            action->setBounds (area.removeFromRight (getHeight()).reduced (2));
    }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        const auto& p = getPalette (*this);
        auto area = getLocalBounds();
        const auto lit = isHighlighted || isDown;

        (getToggleState() ? open : folded)
            .draw (g,
                   area.removeFromLeft ((int) History::sectionChevronSize)
                       .toFloat()
                       .withSizeKeepingCentre (History::sectionChevronSize, History::sectionChevronSize),
                   lit ? p.text : p.textMuted);
        area.removeFromLeft (Space::xs);

        if (count > 0)
            area.removeFromRight (drawBadge (g, area, juce::String (count), p.border, p.text) + Space::xs);

        if (! actions.empty() && actions.front()->isVisible())
            area.removeFromRight ((int) actions.size() * getHeight());

        g.setColour (p.text);
        g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
        const auto headingWidth =
            juce::jmin (area.getWidth(), juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), heading)) + Space::s);
        g.drawText (heading, area.removeFromLeft (headingWidth), juce::Justification::centredLeft, true);
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (detail, area, juce::Justification::centredLeft, true);
    }

private:
    // The pointer on the heading or on an action shows the actions, as VS Code shows a group's.
    struct Hover : public juce::MouseListener
    {
        explicit Hover (Section& s)
            : section (s)
        {
        }

        void mouseEnter (const juce::MouseEvent&) override { section.showActions (true); }
        void mouseExit (const juce::MouseEvent&) override { section.showActions (section.isMouseOver (true)); }

        Section& section;
    };

    juce::String heading, detail;
    int count = 0;
    TintedIcon open { Icon::expanded }, folded { Icon::folded };
    std::vector<std::unique_ptr<IconButton>> actions;
    bool actionsAllowed = true;
    Hover hover { *this };

    void showActions (bool shown)
    {
        for (auto& action : actions)
            action->setVisible (shown && actionsAllowed);

        repaint();
    }
};

//==============================================================================
// Which row of a list is under the pointer, for what shows there only then (a line's actions).
struct VersionsView::RowHover : public juce::MouseListener
{
    RowHover (juce::ListBox& l, std::function<void (int previous, int row)> changed)
        : list (l),
          onChange (std::move (changed))
    {
        list.addMouseListener (this, true);
    }

    ~RowHover() override { list.removeMouseListener (this); }

    void mouseMove (const juce::MouseEvent& e) override { update (e); }
    void mouseEnter (const juce::MouseEvent& e) override { update (e); }
    void mouseDrag (const juce::MouseEvent& e) override { update (e); }
    void mouseExit (const juce::MouseEvent& e) override
    {
        // Out of a row's component into another's is a move; out of the list is none under the pointer.
        if (! list.getLocalBounds().contains (e.getEventRelativeTo (&list).getPosition()))
            setRow (-1);
    }

    int row = -1;

private:
    juce::ListBox& list;
    std::function<void (int, int)> onChange;

    void update (const juce::MouseEvent& e)
    {
        const auto position = e.getEventRelativeTo (&list).getPosition();
        setRow (list.getRowContainingPosition (position.x, position.y));
    }

    void setRow (int newRow)
    {
        if (newRow != row)
            onChange (std::exchange (row, newRow), newRow);
    }
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
        auto area = juce::Rectangle<int> (width, height);

        if (selected || row == getHover().row)
        {
            g.setColour (selected ? p.accent.withAlpha (selectionWash) : p.text.withAlpha (History::hoverWash));
            g.fillRect (area);
        }

        area.reduce (Space::s, 0);

        if (! isStaged && view.lines == Lines::merge)
        {
            // A box ticked while the merge takes the line, its letter, and a word where it conflicts with this branch's change.
            const auto& line = view.mergeLines[(size_t) row];
            const auto colour = getChangeColour (getKind (line.change), theme);
            const auto box = area.removeFromLeft (height).toFloat().withSizeKeepingCentre (History::tickSize, History::tickSize);
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
            drawLetter (g, area, "C", p.warning);
            drawLine (g, area, p.text, p.textMuted, clash.line + ": " + clash.what, describeValue (clash, clash.chosen));
            return;
        }

        // Its icon, its name and what changed, its actions under the pointer, and its letter, in its status's color.
        const auto& change = getLine (row);
        const auto colour = getChangeColour (getKind (change), theme);
        auto& icon = change.isSong ? view.songIcon : change.isMaster ? view.masterIcon : view.trackIcon;
        icon.draw (
            g,
            area.removeFromLeft ((int) History::rowIconSize).toFloat().withSizeKeepingCentre (History::rowIconSize, History::rowIconSize),
            p.textMuted);
        area.removeFromLeft (Space::xs);
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
    RowHover& getHover() const { return isStaged ? *view.stagedHover : *view.changesHover; }
};

//==============================================================================
// The graph: the uncommitted changes, the song as it is, then every branch's commits, newest first, each a dot on its
// branch's line, in its branch's column, and on a branch's last commit the branch's name.
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

        if (selected)
        {
            g.setColour (p.accent.withAlpha (selectionWash));
            g.fillRect (area);
        }

        area.reduce (Space::s, 0);
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

        // Its play mark, while any commit has a bounce: what is heard in the accent's color.
        if (view.anyBounce)
        {
            const auto mark =
                area.removeFromRight (History::playWidth).toFloat().withSizeKeepingCentre (History::playSize, History::playSize);

            if (view.hasPlayMark (row))
            {
                const auto heard = row == 0 ? view.heardVersion.isEmpty() : view.versions[(size_t) row - 1].id == view.heardVersion;
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

        if (! juce::isPositiveAndBelow (row - 1, (int) view.versions.size()))
            return;

        const auto& version = view.versions[(size_t) row - 1];
        g.setColour (view.getBranchColour (version.branch));
        g.fillEllipse (dotAt (version.branch));

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

        return view.describeHistoryRow (row) + ". Right-click for what can be done with it." + play;
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        selectedRowsChanged (row);

        // On its play mark: it is heard (the uncommitted changes': the song again).
        if (! e.mods.isPopupMenu() && view.hasPlayMark (row)
            && view.getPlayMarkPosition (row).contains (e.getEventRelativeTo (&view.historyList).getPosition()))
        {
            hearVersion (view.context, row == 0 ? juce::String() : view.versions[(size_t) row - 1].id);
            return;
        }

        if (! e.mods.isPopupMenu())
            return;

        if (row > 0)
            view.showMenu ({ "version.hear", "version.bringBack", "version.takeFrom", "version.newBranch", "version.merge",
                             "version.bounce", "version.deleteBounce", "version.exportDawProject" });
        else
            view.showMenu ({ "version.hear", "version.newBranch", "version.exportDawProject" });
    }

    void selectedRowsChanged (int row) override
    {
        // The uncommitted changes, or a commit, which the commands act on.
        const auto isVersion = juce::isPositiveAndBelow (row - 1, (int) view.versions.size());
        selectVersion (view.context, isVersion ? view.versions[(size_t) row - 1].id : juce::String());
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
      messageField (std::make_unique<MessageField>()),
      stagedSection (std::make_unique<Section> ("Staged Changes")),
      changesSection (std::make_unique<Section> ("Changes")),
      historySection (std::make_unique<Section> ("Graph")),
      stagedModel (std::make_unique<ChangesModel> (*this, true)),
      changesModel (std::make_unique<ChangesModel> (*this, false)),
      historyModel (std::make_unique<HistoryModel> (*this)),
      stagedList ("Staged Changes", stagedModel.get()),
      changesList ("Changes", changesModel.get()),
      historyList ("Graph", historyModel.get())
{
    setTitle ("Source Control");
    const auto& p = getPalette (*this);

    title.setText ("Source Control", juce::dontSendNotification);
    title.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
    title.setJustificationType (juce::Justification::centredLeft);
    title.setMinimumHorizontalScale (1.0f);

    for (auto* label : { &emptyText, &roomText })
    {
        label->setFont (juce::FontOptions (TextSize::small));
        label->setJustificationType (juce::Justification::topLeft);
        label->setColour (juce::Label::textColourId, p.textMuted);
    }

    roomText.setTitle ("Room the history takes");
    branchButton->onClick = [this] { commands.invoke ("version.pickBranch"); };
    moreButton.setQuiet (true);
    moreButton.onClick = [this] { makeMoreMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&moreButton)); };

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

    for (auto* section : { stagedSection.get(), changesSection.get(), historySection.get() })
        section->onClick = [this] { resized(); };

    doneButton.setButtonText ("Done");
    doneButton.onClick = [this] { commands.invoke ("version.showChanges"); };
    mergeButton.setButtonText ("Merge");
    mergeButton.onClick = [this] { commands.invoke ("version.finishMerge"); };
    cancelButton.setButtonText ("Abort");
    cancelButton.onClick = [this] { commands.invoke ("version.cancelMerge"); };

    for (auto* list : { &stagedList, &changesList, &historyList })
    {
        list->setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list->setTitle (list->getName());
    }

    stagedList.setRowHeight (History::changeRowHeight);
    changesList.setRowHeight (History::changeRowHeight);
    historyList.setRowHeight (History::versionRowHeight);

    const auto repaintRows = [] (juce::ListBox& list)
    {
        return [&list] (int previous, int row)
        {
            list.repaintRow (previous);
            list.repaintRow (row);
        };
    };

    stagedHover = std::make_unique<RowHover> (stagedList, repaintRows (stagedList));
    changesHover = std::make_unique<RowHover> (changesList, repaintRows (changesList));

    for (auto* c : std::initializer_list<juce::Component*> { &title, branchButton.get(), &moreButton, messageField.get(), &saveButton,
                                                             &saveMenuButton, changesSection.get(), &changesList, historySection.get(),
                                                             &historyList })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &emptyText, &doneButton, &mergeButton, &cancelButton, &roomText,
                                                             stagedSection.get(), &stagedList })
        addChildComponent (c);

    refreshTooltips();
    context.addListener (this);
    refresh();
}

VersionsView::~VersionsView()
{
    stagedHover = nullptr;
    changesHover = nullptr;
    context.removeListener (this);
}

juce::TextEditor& VersionsView::getMessageField() noexcept
{
    return *messageField;
}

juce::Button& VersionsView::getBranchButton() noexcept
{
    return *branchButton;
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
    cancelButton.setTooltip (withShortcut (commands, "Closes the merge's list, taking nothing", "version.cancelMerge"));
    branchButton->setTooltip (
        withShortcut (commands, "The branch the song is on: click to check out another, or to create one", "version.pickBranch"));
    moreButton.setTooltip ("More Actions: commit, changes, branches, DAWproject, clean up");
    historyList.setTooltip ("Every commit, newest first, each on its branch's line");
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
    addCommandItems (commitMenu, commands, { "version.save", "version.bounceEach" });
    addCommandItems (changesMenu, commands, { "version.stageAll", "version.unstageAll", "version.discardAll" });
    addCommandItems (branchMenu, commands, { "version.pickBranch", "version.renameBranch", "version.merge" });
    menu.addSubMenu ("Commit", commitMenu);
    menu.addSubMenu ("Changes", changesMenu);
    menu.addSubMenu ("Branch", branchMenu);
    menu.addSeparator();
    addCommandItems (menu, commands, { "version.exportDawProject", "project.importDawProject" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "project.cleanUpAudio" });
    return menu;
}

bool VersionsView::hasPlayMark (int row) const
{
    if (! anyBounce)
        return false;

    return row == 0 || (juce::isPositiveAndBelow (row - 1, (int) versions.size()) && versions[(size_t) row - 1].hasBounce);
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

void VersionsView::focusMessage()
{
    if (messageField->isShowing())
        messageField->grabKeyboardFocus();
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

    if (! juce::isPositiveAndBelow (row - 1, (int) versions.size()))
        return {};

    const auto& version = versions[(size_t) row - 1];
    juce::String text = version.name + ", " + describeWhen (version.saved, juce::Time::getCurrentTime());

    if (const auto branch = getBranchName (version.branch); branch.isNotEmpty())
        text << ", on " << branch;

    if (version.hasBounce)
        text << (version.id == heardVersion ? ", heard" : ", with a bounce");
    else if (version.id == bouncingVersion)
        text << ", bouncing";

    if (version.note.isNotEmpty())
        text << ". " << version.note;

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

    const auto room = getVersionsRoom (context);
    const auto problem = getBounceProblem (context);
    roomText.setText (problem.isNotEmpty() ? problem
                      : versions.empty()   ? juce::String()
                                           : "Commits take " + juce::File::descriptionOfSizeInBytes (room.songs) + ", bounces "
                                                 + juce::File::descriptionOfSizeInBytes (room.bounces),
                      juce::dontSendNotification);
    roomText.setVisible (roomText.getText().isNotEmpty());

    const auto branch = getCurrentBranch (context);
    currentBranch = branch.has_value() ? branch->id : juce::String();
    refreshHistoryLines (branch.has_value() ? branch->head : juce::String());
    branchButton->setBranch (branch.has_value() ? branch->name : juce::String(),
                             branch.has_value() ? branch->colour : getPalette (*this).textMuted);
    branchButton->setVisible (branch.has_value());

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
    changesSection->setActionsAllowed (lines == Lines::changes && compared.isEmpty());
    doneButton.setVisible (lines == Lines::changes && compared.isNotEmpty());
    mergeButton.setVisible (lines == Lines::merge);
    cancelButton.setVisible (lines == Lines::merge);

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

    const auto onBranch = branch.has_value() ? " on '" + branch->name + "'" : juce::String();
    const auto commitKey =
        formatKey (juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::commandModifier, 0).getTextDescription())
            .replace ("Return", "Enter");
    messageField->setTextToShowWhenEmpty ("Message (" + commitKey + " to commit" + onBranch + ")", getPalette (*this).textMuted);
    messageField->setEnabled (keeps);

    saveButton.setEnabled (canSaveVersion (context));
    saveMenuButton.setEnabled (keeps);
    saveButton.setButtonText (isSavingVersion (context)        ? "Committing..."
                              : getBounceWithVersion (context) ? "Commit and Bounce"
                                                               : "Commit");

    stagedList.updateContent();
    changesList.updateContent();
    historyList.updateContent();
    stagedList.repaint();
    changesList.repaint();
    historyList.repaint();
    resized();
}

void VersionsView::refreshHistoryLines (const juce::String& nowFollows)
{
    // Each commit's lines to the commits it follows, and the uncommitted changes' to the current branch's last commit (or, on a
    // branch without commits yet, the one it was made from).
    std::map<juce::String, int> rows;

    for (size_t i = 0; i < versions.size(); ++i)
        rows[versions[i].id] = (int) i + 1;

    historyLines.clear();

    const auto addLine = [this, &rows] (int row, const juce::String& branch, const juce::String& parent, bool first)
    {
        const auto found = rows.find (parent);

        if (found == rows.end())
            return;

        const auto& parentBranch = versions[(size_t) found->second - 1].branch;
        historyLines.push_back (
            { row, found->second, getLane (branch), getLane (parentBranch), first, getBranchColour (first ? branch : parentBranch) });
    };

    addLine (0, currentBranch, nowFollows, true);

    for (size_t i = 0; i < versions.size(); ++i)
        for (int p = 0; p < versions[i].parents.size(); ++p)
            addLine ((int) i + 1, versions[i].branch, versions[i].parents[p], p == 0);
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
    auto area = getLocalBounds().reduced (Space::s);

    // The title and the view's menu, clear of the panel's buttons in its top right corner; then the branch, on a row of its
    // own, as the panel is narrow.
    auto top = area.removeFromTop (controlHeight);
    top.removeFromRight (Shell::getPanelCornerWidth (3) + Space::xs - Space::s);
    moreButton.setBounds (top.removeFromRight (controlHeight));
    title.setBounds (top);

    if (branchButton->isVisible())
        branchButton->setBounds (area.removeFromTop (controlHeight));

    area.removeFromTop (Space::xs);

    // The message, as tall as its lines as they wrap, up to a few.
    const auto lineHeight = juce::roundToInt (messageField->getFont().getHeight() * 1.2f);
    const auto padding = controlHeight - lineHeight;
    juce::AttributedString text (messageField->getText().isEmpty() ? juce::String ("M") : messageField->getText());
    text.setFont (messageField->getFont());
    juce::TextLayout layout;
    layout.createLayout (text, (float) juce::jmax (1, area.getWidth() - padding));
    const auto messageLines = juce::jlimit (1, History::messageLines, layout.getNumLines());
    messageField->setBounds (area.removeFromTop (padding + lineHeight * messageLines));
    area.removeFromTop (Space::xs);

    auto saveRow = area.removeFromTop (controlHeight);
    saveMenuButton.setBounds (saveRow.removeFromRight (History::saveMenuWidth));
    saveRow.removeFromRight (1); // a hairline between the two parts
    saveButton.setBounds (saveRow);
    area.removeFromTop (Space::s);

    // The room the history takes, at the foot of the view.
    if (roomText.isVisible())
        roomText.setBounds (area.removeFromBottom (controlHeight));

    // The staged changes and the changes, as many rows as they have while open (together up to half of what is left, the
    // staged ones up to half of that), then the graph.
    auto listsRoom = area.getHeight() / 2;

    if (stagedSection->isVisible())
    {
        stagedSection->setBounds (area.removeFromTop (controlHeight));
        const auto rows = stagedSection->getToggleState() ? (int) staged.size() : 0;
        const auto height = juce::jmin (listsRoom / 2, rows * History::changeRowHeight);
        stagedList.setBounds (rows == 0 ? juce::Rectangle<int>() : area.removeFromTop (height));
        listsRoom -= height;
    }

    changesSection->setBounds (area.removeFromTop (controlHeight));
    const auto changesOpen = changesSection->getToggleState();

    if (doneButton.isVisible() && changesOpen)
    {
        auto buttons = area.removeFromTop (controlHeight);
        doneButton.setBounds (buttons.removeFromRight (juce::jmin (buttons.getWidth(), History::buttonWidth)));
        area.removeFromTop (Space::xs);
    }

    if (mergeButton.isVisible() && changesOpen)
    {
        auto buttons = area.removeFromTop (controlHeight);
        cancelButton.setBounds (buttons.removeFromRight (juce::jmin (buttons.getWidth() / 2, History::buttonWidth)));
        buttons.removeFromRight (Space::xs);
        mergeButton.setBounds (buttons.removeFromRight (juce::jmin (buttons.getWidth(), History::buttonWidth)));
        area.removeFromTop (Space::xs);
    }

    for (auto* button : { &doneButton, &mergeButton, &cancelButton })
        if (! changesOpen)
            button->setBounds ({});

    if (emptyText.isVisible() && changesOpen)
        emptyText.setBounds (area.removeFromTop (History::noteHeight + (isMerging (context) ? History::noteHeight : 0)));
    else
        emptyText.setBounds ({});

    const auto rows = changesOpen ? getNumChanges() : 0;
    const auto listHeight = juce::jmin (juce::jmax (listsRoom, History::changeRowHeight), rows * History::changeRowHeight);
    changesList.setBounds (rows == 0 ? juce::Rectangle<int>() : area.removeFromTop (listHeight));
    area.removeFromTop (Space::s);

    historySection->setBounds (area.removeFromTop (controlHeight));
    historyList.setBounds (historySection->getToggleState() ? area : juce::Rectangle<int>());
}
} // namespace phazite::ui
