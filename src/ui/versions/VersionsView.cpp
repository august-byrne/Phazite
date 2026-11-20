// SPDX-License-Identifier: AGPL-3.0-or-later
#include "VersionsView.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <limits>
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

    // A line's letter: A added, M modified, D deleted (C, a merge's conflict, is drawn on its own).
    juce::String getLetter (const VersionChange& change)
    {
        return change.kind == VersionChange::Kind::added ? "A" : change.kind == VersionChange::Kind::removed ? "D" : "M";
    }

    // A line as screen readers say it: "Bass, Modified: volume", "Keys, Added", "Lead, Deleted".
    juce::String describeChangeLine (const VersionChange& change)
    {
        const juce::String status = change.kind == VersionChange::Kind::added     ? "Added"
                                    : change.kind == VersionChange::Kind::removed ? "Deleted"
                                                                                  : "Modified";
        return change.name + ", " + status + (change.what.isEmpty() ? juce::String() : ": " + change.what);
    }

    // A conflict's value, by whose it is: as before either branch changed it, this branch's, or the other's.
    juce::String describeValue (const ClashInfo& clash, int value)
    {
        const juce::String whose[] = { "Before", "This branch", clash.otherBranch };
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

    // A group of automatic saves, as its row says it: "6 automatic saves".
    juce::String describeAutomaticSaves (int count)
    {
        return juce::String (count) + " automatic saves";
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
// The version's name and note: Enter starts a new line, Ctrl+Enter saves the version, as in VS Code.
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
// A list of lines: the changes, each to discard, a click selecting a line's track; a merge's lines, what it will take; or a
// merge's conflicts, a click choosing a value.
class VersionsView::ChangesModel : public juce::ListBoxModel
{
public:
    explicit ChangesModel (VersionsView& v)
        : view (v)
    {
    }

    int getNumRows() override { return view.getNumChanges(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;

        const auto& p = getPalette (view);
        const auto theme = getThemeOf (view);
        drawListRow (g, view, juce::Rectangle<int> (width, height).toFloat(), selected, row == view.changesHover->row);
        auto area = getRowContent (width, height);

        if (view.lines == Lines::merge)
        {
            // Its icon, its letter, and a word where it conflicts with this branch's change.
            const auto& line = view.mergeLines[(size_t) row];
            const auto colour = getChangeColour (getKind (line.change), theme);
            auto& icon = line.change.isSong ? view.songIcon : line.change.isMaster ? view.masterIcon : view.trackIcon;
            icon.draw (g, takeIconColumn (area, History::rowIconSize), p.textMuted);
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

        if (view.lines == Lines::clashes)
        {
            // A conflict, its letter C.
            const auto& clash = view.clashes[(size_t) row];
            takeIconColumn (area, History::rowIconSize);
            drawLetter (g, area, "C", p.warning);
            drawLine (g, area, p.text, p.textMuted, clash.line + ": " + clash.what, describeValue (clash, clash.chosen));
            return;
        }

        // Its icon, its name and what changed, its Discard Changes under the pointer, and its letter, in its status's color.
        const auto& change = view.changes[(size_t) row];
        const auto colour = getChangeColour (getKind (change), theme);
        auto& icon = change.isSong ? view.songIcon : change.isMaster ? view.masterIcon : view.trackIcon;
        icon.draw (g, takeIconColumn (area, History::rowIconSize), p.textMuted);
        drawLetter (g, area, getLetter (change), colour);

        if (view.showsActions (row))
            view.discardIcon.draw (
                g, area.removeFromRight (History::actionWidth).toFloat().withSizeKeepingCentre (History::rowIconSize, History::rowIconSize),
                p.text);

        drawLine (g, area, colour, p.textMuted, change.name, change.what, change.kind == VersionChange::Kind::removed);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()) || view.lines == Lines::merge)
            return;

        if (view.lines == Lines::clashes)
        {
            view.showClashMenu (row);
            return;
        }

        // A click selects what changed: its track, or the master; on its arrow, discards it; the right button shows what can
        // be done with it.
        const auto change = view.changes[(size_t) row];
        const auto at = e.getEventRelativeTo (&view.changesList).getPosition();
        selectChange (view.context, change);

        // The command for the line selected, which gets the plug-ins' states first (known bug 166).
        if (! e.mods.isPopupMenu() && view.getPutBackPosition (row).contains (at))
        {
            view.commands.invoke ("version.putBack");
            return;
        }

        if (change.track.isValid() && change.kind != VersionChange::Kind::removed)
            view.context.getSongSelection().selectOnly (change.track);

        if (e.mods.isPopupMenu())
            view.showMenu ({ "version.putBack" });
    }

    void selectedRowsChanged (int row) override
    {
        if (juce::isPositiveAndBelow (row, getNumRows()) && view.lines == Lines::changes)
            selectChange (view.context, view.changes[(size_t) row]);

        view.changesList.repaint(); // the actions on the row selected now
    }

    void returnKeyPressed (int row) override
    {
        if (view.lines == Lines::clashes)
            view.showClashMenu (row);
    }

    juce::String getNameForRow (int row) override { return view.describeChange (row); }

    juce::String getTooltipForRow (int row) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return {};

        if (view.lines == Lines::merge)
            return view.describeChange (row) + ". Merge takes it into the song, with every other line listed.";

        if (view.lines == Lines::clashes)
            return view.describeChange (row) + ". Both branches changed it: click to choose whose value the song has.";

        return view.describeChange (row) + ". Click to select it in the song; its arrow discards it.";
    }

private:
    VersionsView& view;
};

//==============================================================================
// The graph: the changes since the last version, the song as it is, then every branch's versions, newest first, each a dot on its
// branch's line, in its branch's column, and on a branch's last version the branch's name; under a version opened, what it
// changed, a row a line. Between the versions, by when each was taken, the automatic saves in grey: a ring in its branch's
// column, or a group's row for those between two versions, with a row for each under it while it is unfolded.
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

        // The automatic saves, in grey: when each was taken at its row's end, where the versions' times are (before the play
        // marks' column, while any version has a bounce).
        const auto drawWhen = [&g, &p, &area, this] (juce::int64 time)
        {
            if (view.anyBounce)
                area.removeFromRight (History::playWidth);

            g.setColour (p.textMuted);
            g.setFont (juce::FontOptions (TextSize::small));
            const auto when = describeWhen (juce::Time (time), juce::Time::getCurrentTime());
            g.drawText (when,
                        area.removeFromRight (juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), when)) + Space::xs),
                        juce::Justification::centredRight, false);
        };

        // Those between two versions, folded into one row: its chevron, how many they are, and when the newest was taken.
        if (const auto* group = view.getRowGroup (row))
        {
            (view.unfoldedGroups.count (group->since) > 0 ? view.openIcon : view.foldedIcon)
                .draw (
                    g,
                    area.removeFromLeft (SideBar::chevronSize).toFloat().withSizeKeepingCentre (History::rowIconSize, History::rowIconSize),
                    p.textMuted);
            drawWhen (view.snapshots[(size_t) group->first].time);
            g.setColour (p.textMuted);
            g.setFont (juce::FontOptions (TextSize::body));
            g.drawText (describeAutomaticSaves (group->count), area, juce::Justification::centredLeft, true);
            return;
        }

        // One of them: a ring on its branch's line, its clock, why it was taken (and its branch, where that isn't the song's),
        // and its Restore under the pointer; one level in under its group's row.
        if (const auto* snapshot = view.getRowSnapshot (row))
        {
            if (view.getBranchName (snapshot->branchID).isNotEmpty())
            {
                g.setColour (view.getBranchColour (snapshot->branchID));
                g.drawEllipse (dotAt (snapshot->branchID).reduced (History::lineWidth), History::lineWidth / 2.0f);
            }

            if (view.historyRows[(size_t) row].group >= 0)
                area.removeFromLeft (SideBar::chevronSize + SideBar::indent);

            view.clockIcon.draw (g, takeIconColumn (area, History::rowIconSize), p.textMuted);
            drawWhen (snapshot->time);

            if (view.historyHover != nullptr && (row == view.historyHover->row || selected))
                view.discardIcon.draw (g,
                                       area.removeFromRight (History::actionWidth)
                                           .toFloat()
                                           .withSizeKeepingCentre (History::rowIconSize, History::rowIconSize),
                                       p.text);

            drawLine (g, area, p.textMuted, p.textMuted, snapshot->reason,
                      snapshot->branchID != view.currentBranch && snapshot->branch.isNotEmpty() ? "on " + snapshot->branch
                                                                                                : juce::String());
            return;
        }

        // A line under its version, one level in from the version's name: its icon, its name and what changed, its Restore under
        // the pointer, and its letter.
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

        // Its play mark, while any version has a bounce: what is heard in the accent's color.
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
            g.drawText ("Changes Since the Last Version", area, juce::Justification::centredLeft, true);
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

        // The version open in the song's place, in plain words, in the accent's colour.
        if (version.id == view.shownVersion)
            area.removeFromRight (drawBadge (g, area, "Open", p.accent, p.background) + Space::xs);

        // The names of the branches whose last version it is: the current one's filled.
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
            return "Changes Since the Last Version: the song as it is. Right-click to create a branch from it." + juce::String (play);

        if (const auto* version = view.getLineVersion (row))
            return view.describeHistoryRow (row) + ". Its arrow restores it from \"" + version->name + "\"; right-click for more.";

        if (view.getRowGroup (row) != nullptr)
            return view.describeHistoryRow (row)
                   + ": the song as it was at times between these versions, kept without asking. Click to show or fold them.";

        if (view.getRowSnapshot (row) != nullptr)
            return view.describeHistoryRow (row)
                   + ": the song as it was then, kept without asking. Its arrow restores the song to it, as one undo step; "
                     "right-click for more.";

        return view.describeHistoryRow (row)
               + ". Click to show what it changed, double-click to open it in place of the song, live; right-click for what can be "
                 "done with it."
               + play;
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        selectedRowsChanged (row);
        const auto at = e.getEventRelativeTo (&view.historyList).getPosition();

        if (e.mods.isPopupMenu())
        {
            if (auto menu = view.makeHistoryMenu (row); menu.getNumItems() > 0)
                menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());

            return;
        }

        // A group of automatic saves unfolds, or folds; one of them: its arrow restores the song to it, as one undo step.
        if (view.getRowGroup (row) != nullptr)
        {
            view.unfoldAutomaticSaves (row);
            return;
        }

        if (view.getRowSnapshot (row) != nullptr)
        {
            if (view.getSnapshotRestorePosition (row).contains (at))
                view.commands.invoke ("version.restoreSnapshot");

            return;
        }

        // On its play mark: it is heard (the changes' row: the song again).
        if (view.hasPlayMark (row) && view.getPlayMarkPosition (row).contains (at))
        {
            const auto* version = view.getRowVersion (row);
            hearVersion (view.context, version != nullptr ? version->id : juce::String());
            return;
        }

        // A line under a version: its arrow restores it from that version, as one undo step; a click selects its track.
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

        // A version unfolds, or folds.
        if (const auto* version = view.getRowVersion (row))
            view.unfoldVersion (version->id);
    }

    // A double-click on a version opens it in the song's place, live (Open Version): what a producer asks of an old version.
    // On the changes' row, Now again.
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        if (view.getRowVersion (row) != nullptr)
            view.commands.invoke ("version.open");
        else if (row == 0)
            view.commands.invoke ("version.backToNow");
    }

    void returnKeyPressed (int row) override
    {
        if (const auto* version = view.getRowVersion (row))
            view.unfoldVersion (version->id);
        else if (view.getRowGroup (row) != nullptr)
            view.unfoldAutomaticSaves (row);
    }

    void selectedRowsChanged (int row) override
    {
        // The changes' row, a version, or a line under one, which the commands act on.
        if (const auto* change = view.getRowChange (row))
            selectVersionChange (view.context, view.getLineVersion (row)->id, *change);
        else if (const auto* version = view.getRowVersion (row))
            selectVersion (view.context, version->id);
        else
            selectVersion (view.context, {});

        // Or an automatic save, which version.restoreSnapshot restores (selected after the version, which selects none); or
        // a group of them, which the view keeps selected as its rows move.
        const auto* snapshot = view.getRowSnapshot (row);
        const auto* group = view.getRowGroup (row);
        selectSnapshot (view.context, snapshot != nullptr ? snapshot->time : 0);
        view.selectedGroup = group != nullptr ? std::optional (group->since) : std::nullopt;

        view.historyList.repaint(); // the actions on the row selected now
    }

private:
    VersionsView& view;
};

//==============================================================================
// Undo History: the song's undo steps newest first, those undone dimmed with Redo's arrow above the one the song is at, which
// says "now" at its end, and under the oldest, the song before them. A click on a row goes back or forth to it; the keys move
// the selection, and Return goes to the row selected. The row selected is the one the song is at, until the keys move it.
class VersionsView::UndoHistoryModel : public juce::ListBoxModel
{
public:
    explicit UndoHistoryModel (VersionsView& v)
        : view (v)
    {
    }

    int getNumRows() override { return view.getNumUndoRows(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;

        const auto& p = getPalette (view);
        const auto now = view.getUndoNowRow();
        drawListRow (g, view, juce::Rectangle<int> (width, height).toFloat(), selected,
                     view.undoHover != nullptr && row == view.undoHover->row);
        auto area = getRowContent (width, height);

        if (row == now)
        {
            g.setColour (p.textMuted);
            g.setFont (juce::FontOptions (TextSize::small));
            const juce::String nowText ("now");
            g.drawText (
                nowText,
                area.removeFromRight (juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), nowText)) + Space::xs),
                juce::Justification::centredRight, false);
        }

        const auto icon = takeIconColumn (area, History::rowIconSize);

        if (row < now)
            view.redoIcon.draw (g, icon, p.textMuted);

        g.setColour (row < now || row == getNumRows() - 1 ? p.textMuted : p.text);
        g.setFont (juce::FontOptions (TextSize::body));
        g.drawText (getStepName (row), area, juce::Justification::centredLeft, true);
    }

    juce::String getNameForRow (int row) override { return view.describeUndoRow (row); }

    juce::String getTooltipForRow (int row) override
    {
        const auto now = view.getUndoNowRow();
        return row == now                ? juce::String ("The song is here now")
               : row < now               ? juce::String ("Click to redo the steps up to this one")
               : row == getNumRows() - 1 ? juce::String ("Click to undo every step listed")
                                         : juce::String ("Click to undo the steps after this one");
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            view.goToUndoRow (row);
    }

    void returnKeyPressed (int row) override { view.goToUndoRow (row); }

    // A step's name, or what the song was before them all (the last row).
    juce::String getStepName (int row) const
    {
        const auto& steps = view.undoHistory.steps;

        if (row >= (int) steps.size())
            return "Before these steps";

        const auto& name = steps[steps.size() - 1 - (size_t) row];
        return name.isNotEmpty() ? name : juce::String ("A change");
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
      changesSection (std::make_unique<SectionHeader> ("Changes", SectionHeader::Style::group)),
      historySection (std::make_unique<SectionHeader> ("History", SectionHeader::Style::pane)),
      undoSection (std::make_unique<SectionHeader> ("Undo History", SectionHeader::Style::pane)),
      changesModel (std::make_unique<ChangesModel> (*this)),
      historyModel (std::make_unique<HistoryModel> (*this)),
      undoModel (std::make_unique<UndoHistoryModel> (*this)),
      changesList ("Changes", changesModel.get()),
      historyList ("History", historyModel.get()),
      undoList ("Undo History", undoModel.get())
{
    setTitle ("Versions");

    // Its icon gives it the keys (and a screen reader its name), not the version's name field, so that Space plays on; Tab
    // goes on into its parts.
    setWantsKeyboardFocus (true);

    for (auto* label : { &emptyText, &roomText, &newerText })
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

    // One field, as VS Code's message: its first line names the version, the lines after it are its note.
    messageField->setTitle ("Version name");
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

    // Save Version across the view in the accent color, its menu's part joined at its right end.
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

    // The group's action under the pointer, as VS Code's.
    changesSection->addAction (Icon::undo, "Discard All Changes", [this] { commands.invoke ("version.discardAll"); });

    for (auto* section : { changesSection.get(), historySection.get(), undoSection.get() })
        section->onClick = [this] { resized(); };

    undoSection->setToggleState (false, juce::dontSendNotification); // folded at first, its count on its badge
    undoSection->onClick = [this]
    {
        resized();

        if (const auto now = getUndoNowRow(); now >= 0)
            undoList.scrollToEnsureRowIsOnscreen (now); // opened, the row the song is at in view
    };

    doneButton.setButtonText ("Done");
    doneButton.onClick = [this] { commands.invoke ("version.showChanges"); };
    mergeButton.setButtonText ("Merge");
    mergeButton.onClick = [this] { commands.invoke ("version.finishMerge"); };
    cancelButton.setButtonText ("Cancel");
    cancelButton.onClick = [this] { commands.invoke ("version.cancelMerge"); };
    newerText.setTitle ("Newer versions");
    newerButton.onClick = [this] { commands.invoke ("version.takeNewer"); };

    // The bar of the version open in the song's place: what it is, and the ways on from it.
    openText.setTitle ("Version open");
    openText.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
    openText.setJustificationType (juce::Justification::topLeft);
    setThemeColour (openText, juce::Label::textColourId, &Palette::focus); // reads as text (4.5:1), as the transport bar's
    backButton.setButtonText ("Back to Now");
    backButton.onClick = [this] { commands.invoke ("version.backToNow"); };
    branchHereButton.setButtonText ("Create Branch Here");
    branchHereButton.onClick = [this]
    {
        selectVersion (context, shownVersion);
        commands.invoke ("version.newBranch");
    };
    openBounceButton.setButtonText ("Hear Its Bounce");
    openBounceButton.onClick = [this]
    {
        selectVersion (context, shownVersion);
        commands.invoke ("version.hear");
    };

    for (auto* list : { &changesList, &historyList, &undoList })
    {
        list->setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list->setTitle (list->getName());
    }

    changesList.setRowHeight (History::changeRowHeight);
    historyList.setRowHeight (History::versionRowHeight);
    undoList.setRowHeight (History::changeRowHeight);

    const auto repaintRows = [] (juce::ListBox& list)
    {
        return [&list] (int previous, int row)
        {
            list.repaintRow (previous);
            list.repaintRow (row);
        };
    };

    changesHover = std::make_unique<ListRowHover> (changesList, repaintRows (changesList));
    historyHover = std::make_unique<ListRowHover> (historyList, repaintRows (historyList));
    undoHover = std::make_unique<ListRowHover> (undoList, repaintRows (undoList));

    for (auto* c :
         std::initializer_list<juce::Component*> { &title, branchButton.get(), messageField.get(), &saveButton, &saveMenuButton,
                                                   changesSection.get(), &changesList, historySection.get(), &historyList, &undoList })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &emptyText, &doneButton, &mergeButton, &cancelButton, &roomText, &newerText,
                                                             &newerButton, undoSection.get(), &openText, &backButton, &branchHereButton,
                                                             &openBounceButton })
        addChildComponent (c);

    refreshTooltips();
    context.addListener (this);
    refresh();
}

VersionsView::~VersionsView()
{
    changesHover = nullptr;
    historyHover = nullptr;
    undoHover = nullptr;
    context.removeListener (this);
}

juce::Button& VersionsView::getUndoHistorySection() noexcept
{
    return *undoSection;
}

int VersionsView::getNumUndoRows() const noexcept
{
    return undoHistory.steps.empty() ? 0 : (int) undoHistory.steps.size() + 1; // the steps, and the song before them
}

int VersionsView::getUndoNowRow() const noexcept
{
    return undoHistory.steps.empty() ? -1 : (int) undoHistory.steps.size() - undoHistory.done;
}

juce::String VersionsView::describeUndoRow (int row) const
{
    if (! juce::isPositiveAndBelow (row, getNumUndoRows()))
        return {};

    const auto now = getUndoNowRow();
    return undoModel->getStepName (row) + (row == now ? ", now" : row < now ? ", undone" : "");
}

void VersionsView::goToUndoRow (int row)
{
    // A row is the song as it was after its step, the last row as it was before them all: so many steps done.
    if (juce::isPositiveAndBelow (row, getNumUndoRows()))
        goToUndoStep (context, (int) undoHistory.steps.size() - row);
}

void VersionsView::refreshUndoHistory()
{
    auto history = getUndoHistory (context);
    const auto shown = context.hasProject();

    if (history == undoHistory && undoSection->isVisible() == shown)
        return;

    undoHistory = std::move (history);
    undoSection->set ("Undo History", {}, (int) undoHistory.steps.size());
    undoSection->setVisible (shown);
    undoList.updateContent();
    resized();

    // The row the song is at, selected and in view.
    if (const auto now = getUndoNowRow(); now >= 0)
        undoList.selectRow (now);
    else
        undoList.deselectAllRows();

    undoList.repaint();
}

juce::String VersionsView::describeSnapshot (int index) const
{
    if (! juce::isPositiveAndBelow (index, (int) snapshots.size()))
        return {};

    // "Automatic save, 5 min, on main"; with why it was taken where that says more: "Automatic save, Before Restore, now".
    const auto& snapshot = snapshots[(size_t) index];
    return "Automatic save, " + (snapshot.reason != "Auto-saved" ? snapshot.reason + ", " : juce::String())
           + describeWhen (juce::Time (snapshot.time), juce::Time::getCurrentTime())
           + (snapshot.branch.isNotEmpty() ? ", on " + snapshot.branch : juce::String());
}

const SnapshotInfo* VersionsView::getRowSnapshot (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) historyRows.size()))
        return nullptr;

    const auto index = historyRows[(size_t) row].snapshot;
    return juce::isPositiveAndBelow (index, (int) snapshots.size()) ? &snapshots[(size_t) index] : nullptr;
}

const VersionsView::SnapshotGroup* VersionsView::getRowGroup (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) historyRows.size()))
        return nullptr;

    const auto& place = historyRows[(size_t) row];
    return place.snapshot < 0 && juce::isPositiveAndBelow (place.group, (int) snapshotGroups.size()) ? &snapshotGroups[(size_t) place.group]
                                                                                                     : nullptr;
}

int VersionsView::getSnapshotRow (int index) const
{
    for (int row = 0; row < (int) historyRows.size(); ++row)
        if (index >= 0 && historyRows[(size_t) row].snapshot == index)
            return row;

    return -1;
}

int VersionsView::getSnapshotGroupRow (int index) const
{
    for (int row = 0; row < (int) historyRows.size(); ++row)
        if (const auto* group = getRowGroup (row); group != nullptr && index >= group->first && index < group->first + group->count)
            return row;

    return -1;
}

void VersionsView::unfoldAutomaticSaves (int row)
{
    const auto* group = getRowGroup (row);

    if (group == nullptr)
        return;

    // Another click folds it. Its row stays where it is, and selected.
    const auto since = group->since;

    if (unfoldedGroups.erase (since) == 0)
        unfoldedGroups.insert (since);

    selectSnapshot (context, 0); // one of its rows, selected, goes out of sight as it folds
    selectedGroup = since;
    refreshHistoryRows();
    refreshHistoryLines();
    historyList.updateContent();
    historyList.selectRow (row, true, true);
    historyList.repaint();
}

juce::Rectangle<int> VersionsView::getSnapshotRestorePosition (int row) const
{
    // Before the time at its end (HistoryModel::paintListBoxItem()), under the pointer or on the row selected.
    const auto* snapshot = getRowSnapshot (row);

    if (snapshot == nullptr || historyHover == nullptr || (row != historyHover->row && ! historyList.isRowSelected (row)))
        return {};

    const auto when = describeWhen (juce::Time (snapshot->time), juce::Time::getCurrentTime());
    const auto whenWidth =
        juce::roundToInt (juce::TextLayout::getStringWidth (juce::Font (juce::FontOptions (TextSize::small)), when)) + Space::xs;
    return historyList.getRowPosition (row, true)
        .withTrimmedRight (Space::s + (anyBounce ? History::playWidth : 0) + whenWidth)
        .removeFromRight (History::actionWidth);
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
                                         "Saves the project and a version of the song as it is, named by the field's first line, or "
                                         "by what changed",
                                         "version.save"));
    saveMenuButton.setTooltip ("Save Version's choices: whether each version is bounced as it is saved");
    messageField->setTooltip ("The version's name: its first line is the name, the lines after it a note. Left empty, the version is "
                              "named by what changed. Ctrl+Enter saves it");
    changesList.setTooltip ("What changed in the song since the last version, by track. A added, M modified, D deleted");
    doneButton.setTooltip (withShortcut (commands, "Lists the changes since the last version again", "version.showChanges"));
    newerButton.setTooltip (
        withShortcut (commands, "Brings the branch's newer versions into the song, as one undo step", "version.takeNewer"));
    mergeButton.setTooltip (withShortcut (commands, "Takes every line listed into the song, as one undo step", "version.finishMerge"));
    cancelButton.setTooltip (withShortcut (commands,
                                           "Cancels the merge: closes its list, taking nothing, or, once taken, makes the song what it "
                                           "was before it, as one undo step",
                                           "version.cancelMerge"));
    branchButton->setTooltip (
        withShortcut (commands, "The branch the song is on: click to switch to another, or to create one", "version.pickBranch"));
    moreButton.setTooltip ("More Actions: versions, changes, branches, DAWproject, clean up");
    historySection->setTooltip ("The song's versions: each on its branch's line, where branches part and where they merge");
    historyList.setTooltip (
        "Every version, newest first, each on its branch's line: click one to show what it changed, double-click to open it in the song's "
        "place. In grey between them, the automatic saves: the song as it was at times while it had changes");
    undoSection->setTooltip (
        "The changes made to the song since it opened, as Undo and Redo go through them: click one to go back or forth to it");
    undoList.setTooltip ("Undo History, newest first: the dimmed steps are undone, and Redo would do them again. Click a step to go "
                         "back or forth to it");
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
    juce::PopupMenu menu, versionMenu, changesMenu, branchMenu;
    addCommandItems (versionMenu, commands, { "version.save", "version.undoLast" });
    versionMenu.addSeparator();
    addCommandItems (versionMenu, commands, { "version.bounceEach" });
    addCommandItems (changesMenu, commands, { "version.discardAll" });
    addCommandItems (branchMenu, commands,
                     { "version.takeNewer", "version.pickBranch", "version.mergeBranch", "version.renameBranch", "version.deleteBranch" });
    menu.addSubMenu ("Version", versionMenu);
    menu.addSubMenu ("Changes", changesMenu);
    menu.addSubMenu ("Branch", branchMenu);
    menu.addSeparator();
    addCommandItems (menu, commands, { "version.showAutomatic" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "version.exportDawProject", "project.importDawProject" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "project.cleanUpAudio", "version.verify", "version.recover", "version.resetToReadable" });
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
    // The version opened, as long as it is there: what it changed under it.
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
    snapshotGroups.clear();
    historyRows.push_back ({});

    // The automatic saves taken after a version was saved (on whichever branch), before its row, newest first as they are
    // listed, so that the rows go back in time from the top: one alone as a row of its own; more as their group's row, and a row each under it while it is unfolded. None while they are switched off.
    const auto shown = settings.get (Settings::versionShowAutomatic);
    int next = 0;
    const auto addSnapshotsSince = [this, shown, &next] (juce::int64 time, const juce::String& versionID)
    {
        const auto first = next;

        while (next < (int) snapshots.size() && snapshots[(size_t) next].time > time)
            ++next;

        if (! shown || next == first)
            return;

        if (next == first + 1)
        {
            historyRows.push_back ({ .snapshot = first });
            return;
        }

        const auto group = (int) snapshotGroups.size();
        snapshotGroups.push_back ({ first, next - first, versionID });
        historyRows.push_back ({ .group = group });

        if (unfoldedGroups.count (versionID) > 0)
            for (int i = first; i < next; ++i)
                historyRows.push_back ({ .snapshot = i, .group = group });
    };

    for (int i = 0; i < (int) versions.size(); ++i)
    {
        addSnapshotsSince (versions[(size_t) i].saved.toMilliseconds(), versions[(size_t) i].id);
        historyRows.push_back ({ .version = i });

        if (versions[(size_t) i].id == openedVersion)
            for (int c = 0; c < (int) openedChanges.size(); ++c)
                historyRows.push_back ({ .version = i, .change = c });
    }

    addSnapshotsSince (std::numeric_limits<juce::int64>::min(), {}); // those older than every version
}

void VersionsView::selectHistoryRow()
{
    // A version selected from elsewhere (the bar of the version open, a picker) takes the selection from a group of automatic
    // saves, as the model takes it from an automatic save (selectVersion()).
    if (getSelectedVersion (context).isNotEmpty())
        selectedGroup.reset();

    // An automatic save selected, wherever its row went. Folded into a group since (another was taken beside it), its
    // group's row is selected in its place; gone (thinned away, or Show Automatic Saves switched off), nothing of it stays
    // selected, so that its command is never there for a row nobody sees.
    if (const auto time = getSelectedSnapshot (context); time != 0)
    {
        const auto found = std::find_if (snapshots.begin(), snapshots.end(), [time] (const SnapshotInfo& s) { return s.time == time; });
        const auto index = found != snapshots.end() ? (int) (found - snapshots.begin()) : -1;

        if (const auto row = getSnapshotRow (index); row >= 0)
        {
            if (historyList.getSelectedRow() != row && historyList.getNumSelectedRows() > 0)
                historyList.selectRow (row, true, true);

            return;
        }

        selectSnapshot (context, 0);

        if (const auto* group = getRowGroup (getSnapshotGroupRow (index)))
            selectedGroup = group->since;
    }

    // A group of them selected, wherever its row went.
    if (selectedGroup.has_value())
    {
        for (int row = 0; row < (int) historyRows.size(); ++row)
        {
            if (const auto* group = getRowGroup (row); group != nullptr && group->since == *selectedGroup)
            {
                if (historyList.getSelectedRow() != row && historyList.getNumSelectedRows() > 0)
                    historyList.selectRow (row, true, true);

                return;
            }
        }

        selectedGroup.reset(); // its group went (a version saved since it was taken, its saves thinned to one, or all hidden)
    }

    // What the model selects, wherever its row went: a line under a version, the version, or the changes' row.
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

void VersionsView::unfoldVersion (const juce::String& versionID)
{
    // Another click folds it; one version unfolds at a time.
    openedVersion = openedVersion == versionID ? juce::String() : versionID;
    refreshHistoryRows();
    refreshHistoryLines();
    historyList.updateContent();

    if (const auto row = getHistoryRow (versionID); row >= 0)
        historyList.selectRow (row, true, true);

    historyList.repaint();
}

juce::Rectangle<int> VersionsView::getRestorePosition (int row) const
{
    // Before the letter of a line under its version, inside the row's margin (HistoryModel::paintListBoxItem()).
    if (getRowChange (row) == nullptr || historyHover == nullptr || (row != historyHover->row && ! historyList.isRowSelected (row)))
        return {};

    return historyList.getRowPosition (row, true).withTrimmedRight (Space::s + History::letterWidth).removeFromRight (History::actionWidth);
}

juce::PopupMenu VersionsView::makeHistoryMenu (int row)
{
    // Grouped: opening it, live; going back to it; branching and merging; its name; its bounce, and hearing that; and exporting
    // it. A line under a version: restoring it. The changes' row: back to Now, or to the song from a bounce heard.
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

    // An automatic save: restoring the song to it. A group of them: nothing, as a click unfolds it.
    if (getRowSnapshot (row) != nullptr)
    {
        group ({ "version.restoreSnapshot" });
        return menu;
    }

    if (getRowGroup (row) != nullptr)
        return menu;

    if (const auto* version = getRowVersion (row))
    {
        const auto last = getLastVersion (context);
        group ({ version->id == getOpenVersion (context) ? "version.backToNow" : "version.open" });
        group ({ "version.bringBack", "version.takeFrom" });
        group ({ "version.newBranch", "version.merge" });

        if (last.has_value() && last->id == version->id)
            group ({ "version.editMessage", "version.undoLast" });
        else
            group ({ "version.editMessage" });

        group ({ "version.hear", "version.bounce", "version.deleteBounce" });
        group ({ "version.exportDawProject" });
        return menu;
    }

    group ({ "version.backToNow", "version.hear" });
    group ({ "version.newBranch" });
    group ({ "version.exportDawProject" });
    return menu;
}

juce::Rectangle<int> VersionsView::getPlayMarkPosition (int row) const
{
    // The last column of the row, inside its margin (HistoryModel::paintListBoxItem()).
    const auto bounds = historyList.getRowPosition (row, true);
    return bounds.withTrimmedRight (Space::s).removeFromRight (History::playWidth);
}

bool VersionsView::showsActions (int row) const
{
    return juce::isPositiveAndBelow (row, (int) changes.size()) && lines == Lines::changes
           && (row == changesHover->row || changesList.isRowSelected (row));
}

juce::Rectangle<int> VersionsView::getPutBackPosition (int row) const
{
    // Before the letter, inside the row's margin (ChangesModel::paintListBoxItem()).
    if (! showsActions (row))
        return {};

    return changesList.getRowPosition (row, true).withTrimmedRight (Space::s + History::letterWidth).removeFromRight (History::actionWidth);
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
        return describeChangeLine (line.change) + (line.clashes ? ", conflict" : "");
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

juce::String VersionsView::describeHistoryRow (int row) const
{
    if (row == 0)
        return heardVersion.isEmpty() && anyBounce ? "Changes Since the Last Version, heard" : "Changes Since the Last Version";

    // A line under its version: "Bass, Modified: volume".
    if (const auto* change = getRowChange (row))
    {
        const juce::String status = change->kind == VersionChange::Kind::added     ? "Added"
                                    : change->kind == VersionChange::Kind::removed ? "Deleted"
                                                                                   : "Modified";
        return change->name + ", " + status + (change->what.isEmpty() ? juce::String() : ": " + change->what);
    }

    // The automatic saves: "6 automatic saves, 5 min, open", "Automatic save, 5 min, on main".
    if (const auto* group = getRowGroup (row))
        return describeAutomaticSaves (group->count) + ", "
               + describeWhen (juce::Time (snapshots[(size_t) group->first].time), juce::Time::getCurrentTime())
               + (unfoldedGroups.count (group->since) > 0 ? ", open" : "");

    if (getRowSnapshot (row) != nullptr)
        return describeSnapshot (historyRows[(size_t) row].snapshot);

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

    // Its three values, the one the song has ticked: a choice is heard at once.
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
    changes = getListedChanges (context);
    mergeLines = getMergeLines (context);
    clashes = getClashes (context);
    versions = getVersions (context);
    branches = getBranches (context);
    snapshots = getSnapshots (context);
    heardVersion = getHeardVersion (context);
    anyBounce = std::any_of (versions.begin(), versions.end(), [] (const VersionInfo& version) { return version.hasBounce; });
    bouncingVersion.clear();
    refreshOpenBar();

    for (const auto& version : versions)
    {
        if (const auto progress = getBounceProgress (context, version.id))
        {
            bouncingVersion = version.id;
            bounceProgress = *progress;
        }
    }

    // Bounce Each Version, as the settings keep it, for the song open now; and the room the history takes, or why an old
    // version's bounce failed, until the next one.
    if (const auto bounces = settings.get (Settings::versionBounce); getBounceWithVersion (context) != bounces)
        setBounceWithVersion (context, bounces);

    // What needs saying first: a first save copying the versions (why saving a version, merging and switching are greyed), a
    // branch whose last version can't be read, a bounce or a restore that failed in part, a merge that waits otherwise than
    // it was taken, what Verify History found, a line that went its own way on another computer; else the room the history
    // takes.
    const auto room = getVersionsRoom (context);
    const auto savingProject = isSavingProjectFolder (context);
    juce::String notice;

    for (const auto& text : { savingProject ? juce::String ("Saving the project: saving a version, merging and switching wait until it "
                                                            "is saved.")
                                            : juce::String(),
                              getSaveProblem (context), getReadingProblem (context), getHeadProblem (context), getBaseProblem (context),
                              getBounceProblem (context), getRestoreProblem (context), getMergeProblem (context),
                              describeHistoryCheck (context), describeDivergence (context) })
        if (notice.isEmpty())
            notice = text;

    roomText.setText (notice.isNotEmpty() ? notice
                      : versions.empty()  ? juce::String()
                                          : "Versions take " + juce::File::descriptionOfSizeInBytes (room.songs) + ", bounces "
                                                + juce::File::descriptionOfSizeInBytes (room.bounces),
                      juce::dontSendNotification);
    roomText.setVisible (roomText.getText().isNotEmpty());

    // The branch's versions the song didn't grow from, said under Save Version, with the button that takes them in.
    const auto newer = describeNewerVersions (context);
    newerText.setText (newer, juce::dontSendNotification);
    newerText.setVisible (newer.isNotEmpty());
    newerButton.setButtonText (describeTakeNewer (context));
    newerButton.setVisible (newer.isNotEmpty() && canTakeNewerVersions (context));

    // The changes' row follows the version the song grew from.
    const auto branch = getCurrentBranch (context);
    const auto base = getBaseVersion (context);
    currentBranch = branch.has_value() ? branch->id : juce::String();
    changesFollow = base.has_value() ? base->id : branch.has_value() ? branch->head : juce::String();
    refreshHistoryRows();
    refreshHistoryLines();
    branchButton->setBranch (branch.has_value() ? branch->name : juce::String(),
                             branch.has_value() ? branch->colour : getPalette (*this).textMuted);
    branchButton->setVisible (branch.has_value());

    // A merge's list while it shows; its conflicts while it waits for its version; otherwise the changes.
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

    historySection->set ("History", {}, 0);
    changesSection->setActionsAllowed (lines == Lines::changes && compared.isEmpty());
    doneButton.setVisible (lines == Lines::changes && compared.isNotEmpty());
    mergeButton.setVisible (lines == Lines::merge);
    cancelButton.setVisible (lines == Lines::merge || isMerging (context)); // a merge waiting for its version can be cancelled too

    // What there is to say while there is nothing to list, and while a merge waits for its version.
    juce::String said = ! keeps                                         ? juce::String ("Versions start once the project has a file.")
                        : reading                                       ? juce::String ("Reading the versions...")
                        : ! last                                        ? juce::String ("No versions yet.")
                        : getNumChanges() == 0 && compared.isNotEmpty() ? juce::String ("The song is as that version was.")
                        : getNumChanges() == 0                          ? juce::String ("No changes since the last version.")
                                                                        : juce::String();

    if (isMerging (context))
    {
        said = "Merging \"" + merged + "\": save a version to finish." + (said.isNotEmpty() ? " " + said : juce::String());

        for (const auto& note : getMergeNotes (context))
            said << "\n" << note;
    }

    emptyText.setText (said, juce::dontSendNotification);
    emptyText.setVisible (said.isNotEmpty());

    // The draft the model holds, as one text, when it changed there (empty again once a version was saved with it, or
    // another project's): what is being typed stays as it is.
    if (const auto draft = getDraftMessage(); draft != takenMessage)
    {
        takenMessage = draft;
        messageField->setText (draft, false);
    }

    // While nothing is typed, the name the version would get, as GitHub Desktop shows it ("Update Bass"), with the key that
    // saves it, as VS Code's field says it.
    const auto onBranch = branch.has_value() ? " on '" + branch->name + "'" : juce::String();
    const auto saveKey = formatKey (juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::commandModifier, 0).getTextDescription())
                             .replace ("Return", "Enter");
    const auto next = describeNextCommit (context);
    messageField->setTextToShowWhenEmpty (
        canResetToReadable (context) ? juce::String ("Reset Branch to Last Readable Version to save a version")
        : keeps && ! canSaveOnBranch (context)
            ? (newerButton.isVisible() ? newerButton.getButtonText() : juce::String ("Create Branch")) + " to save a version"
            : (next.isNotEmpty() ? next : juce::String ("Name this version")) + " (" + saveKey + " to save it" + onBranch + ")",
        getPalette (*this).textMuted);
    messageField->setEnabled (keeps);

    saveButton.setEnabled (canSaveVersion (context));
    saveMenuButton.setEnabled (keeps);
    saveButton.setButtonText (savingProject                    ? "Saving the project..."
                              : isSavingVersion (context)      ? "Saving the version..."
                              : getBounceWithVersion (context) ? "Save Version and Bounce"
                                                               : "Save Version");

    changesList.updateContent();
    historyList.updateContent();
    selectHistoryRow();
    changesList.repaint();
    historyList.repaint();
    refreshUndoHistory();
    resized();
}

void VersionsView::refreshOpenBar()
{
    // "Looking at "Verse 2" (3 days ago)", and what of it can't play here (a plug-in not installed), with its bounce where it has
    // one; the Changes under it stay Now's.
    const auto was = std::pair (shownVersion, openText.getText());
    shownVersion = getOpenVersion (context);
    auto text = describeOpenVersion (context);
    const auto missing = getOpenVersionMissingPlugins (context);
    const auto found = std::find_if (versions.begin(), versions.end(), [this] (const VersionInfo& v) { return v.id == shownVersion; });
    const auto hasBounce = found != versions.end() && found->hasBounce;

    if (text.isNotEmpty())
    {
        if (! missing.isEmpty())
            text << ". " << missing.joinIntoString (", ") << (missing.size() == 1 ? " isn't" : " aren't")
                 << " installed here, so it plays nothing" << (hasBounce ? "; its bounce has the whole song." : ".");

        text << "\nThe changes below are the song's own (Now).";
    }

    openText.setText (text, juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &openText, &backButton, &branchHereButton })
        c->setVisible (shownVersion.isNotEmpty());

    // Greyed while the version can't go yet (a render has it), saying why.
    const auto whyNotBack = shownVersion.isNotEmpty() ? whyNotBackToNow (context) : juce::String();
    backButton.setEnabled (whyNotBack.isEmpty());
    backButton.setTooltip (whyNotBack);

    openBounceButton.setVisible (shownVersion.isNotEmpty() && ! missing.isEmpty() && hasBounce);

    if (was != std::pair (shownVersion, openText.getText()))
    {
        historyList.repaint(); // its row's mark
        resized();
    }
}

bool VersionsView::keyPressed (const juce::KeyPress& key)
{
    // Escape, while a version is open: back to Now, as the bar's button.
    if (key == juce::KeyPress::escapeKey && shownVersion.isNotEmpty())
        return commands.invoke ("version.backToNow");

    return false;
}

void VersionsView::refreshHistoryLines()
{
    // Each version's lines to the versions it follows, and the changes' row's to the current branch's last version (or, on a
    // branch without versions yet, the one it was made from).
    // By their rows, which the lines under the version opened push down.
    std::map<juce::String, std::pair<int, juce::String>> rows; // a version's row and branch

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

    addLine (0, currentBranch, changesFollow, true);

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

    // The title and the view's menu; then the version open in the song's place, across the view's top; then the branch, on a
    // row of its own, as the panel is narrow.
    title.setBounds (area.removeFromTop (SideBar::titleHeight));

    if (openText.isVisible())
    {
        const auto textWidth = juce::jmax (1, area.getWidth() - SideBar::fieldLeft - SideBar::fieldRight);
        juce::AttributedString said (openText.getText());
        said.setFont (openText.getFont());
        juce::TextLayout saidLayout;
        saidLayout.createLayout (said, (float) textWidth);
        openText.setBounds (area.removeFromTop (juce::roundToInt (saidLayout.getHeight()) + History::noteHeight)
                                .withTrimmedLeft (SideBar::fieldLeft - openText.getBorderSize().getLeft())
                                .withTrimmedRight (SideBar::fieldRight));

        for (auto* button : { &backButton, &branchHereButton, &openBounceButton })
            if (button->isVisible())
                button->setBounds (inField (area.removeFromTop (SideBar::buttonRowHeight), SideBar::controlHeight));
    }

    if (branchButton->isVisible())
        branchButton->setBounds (area.removeFromTop (ListRow::height).reduced ((int) ListRow::inset, 0));

    // The name field, as tall as its lines as they wrap, up to a few: one line as tall as VS Code's, in its row.
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

    // The branch's newer versions, said under it, and the button that takes them in, across the row.
    if (newerText.isVisible())
        newerText.setBounds (area.removeFromTop (History::noteHeight * 3)
                                 .withTrimmedLeft (SideBar::fieldLeft - newerText.getBorderSize().getLeft())
                                 .withTrimmedRight (SideBar::fieldRight));
    else
        newerText.setBounds ({});

    if (newerButton.isVisible())
        newerButton.setBounds (inField (area.removeFromTop (SideBar::buttonRowHeight), SideBar::controlHeight));
    else
        newerButton.setBounds ({});

    // The room the history takes, at the foot of the view.
    if (roomText.isVisible())
        roomText.setBounds (
            area.removeFromBottom (controlHeight).withTrimmedLeft (SideBar::fieldLeft - roomText.getBorderSize().getLeft()));

    // The changes, as many rows as they have while open (up to half of what is left), then the history.
    const auto listsRoom = area.getHeight() / 2;

    changesSection->setBounds (area.removeFromTop (changesSection->getPreferredHeight()));
    const auto changesOpen = changesSection->getToggleState();

    if (doneButton.isVisible() && changesOpen)
    {
        auto buttons = inField (area.removeFromTop (SideBar::buttonRowHeight), SideBar::controlHeight);
        doneButton.setBounds (buttons.removeFromRight (juce::jmin (buttons.getWidth(), History::buttonWidth)));
    }

    if (cancelButton.isVisible() && changesOpen)
    {
        // Merge and Cancel while a merge's lines show; Cancel alone while it waits for its version.
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

    // Undo History at the foot, its rows (while open) up to a third of what is left, then the history above it.
    const auto placeAtFoot = [&area] (SectionHeader& section, juce::ListBox& list, int numRows)
    {
        if (! section.isVisible())
        {
            section.setBounds ({});
            list.setBounds ({});
            return;
        }

        const auto shown = section.getToggleState() ? numRows : 0;
        const auto height =
            shown == 0 ? 0 : juce::jmin (juce::jmax (History::changeRowHeight, area.getHeight() / 3), shown * History::changeRowHeight);
        list.setBounds (shown == 0 ? juce::Rectangle<int>() : area.removeFromBottom (height));
        section.setBounds (area.removeFromBottom (section.getPreferredHeight()));
    };

    placeAtFoot (*undoSection, undoList, getNumUndoRows());

    historySection->setBounds (area.removeFromTop (historySection->getPreferredHeight()));
    historyList.setBounds (historySection->getToggleState() ? area : juce::Rectangle<int>());
}
} // namespace phazite::ui
