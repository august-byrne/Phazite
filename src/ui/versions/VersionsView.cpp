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
    // A line's name, "+ " for what was added, "- " for what was taken away.
    juce::String getMarkedName (const VersionChange& change)
    {
        return (change.kind == VersionChange::Kind::added ? "+ " : change.kind == VersionChange::Kind::removed ? "- " : "") + change.name;
    }

    juce::String describeChangeLine (const VersionChange& change)
    {
        const auto name = getMarkedName (change);
        return change.what.isEmpty() ? name : name + ": " + change.what;
    }

    juce::String describeValue (const ClashInfo& clash, int value)
    {
        const juce::String whose[] = { "Before both", "This branch", clash.otherBranch };
        return whose[value] + ": " + clash.values[value];
    }

    void setUpHeading (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
        label.setJustificationType (juce::Justification::centredLeft);
        label.setMinimumHorizontalScale (1.0f);
    }

    // A row's name and what it says of it, side by side.
    void drawLine (juce::Graphics& g, juce::Rectangle<int> area, const Palette& p, const juce::String& name, const juce::String& what)
    {
        g.setColour (p.text);
        g.setFont (juce::FontOptions (TextSize::body));
        const auto nameWidth =
            juce::jmin (area.getWidth() / 2, juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), name)) + Space::m);
        g.drawText (name, area.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (what, area, juce::Justification::centredLeft, true);
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
// The list of lines: the changes since the last version, a click selecting a line's track; a merge's lines, a click ticking
// one; or a merge's clashes, a click choosing a value.
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
        if (! juce::isPositiveAndBelow (row, view.getNumChanges()))
            return;

        const auto& p = getPalette (view);
        auto area = juce::Rectangle<int> (width, height).reduced (Space::s, 0);

        if (selected)
        {
            g.setColour (p.accent.withAlpha (selectionWash));
            g.fillRect (0, 0, width, height);
        }

        if (view.lines == Lines::merge)
        {
            // A box ticked while the merge takes the line, and a word where it clashes with what this branch changed.
            const auto& line = view.mergeLines[(size_t) row];
            const auto box = area.removeFromLeft (height).toFloat().withSizeKeepingCentre (History::tickSize, History::tickSize);
            g.setColour (line.ticked ? p.accent : p.textMuted);
            g.drawRoundedRectangle (box.reduced (0.5f), cornerRadius / 2.0f, 1.0f);

            if (line.ticked)
                g.fillRoundedRectangle (box.reduced (History::tickInset), cornerRadius / 2.0f);

            if (line.clashes)
            {
                g.setColour (p.warning);
                g.setFont (juce::FontOptions (TextSize::small));
                const juce::String word ("clashes");
                const auto wordWidth = juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), word)) + Space::s;
                g.drawText (word, area.removeFromRight (wordWidth), juce::Justification::centredRight, false);
            }

            drawLine (g, area, p, getMarkedName (line.change), line.change.what);
            return;
        }

        if (view.lines == Lines::clashes)
        {
            const auto& clash = view.clashes[(size_t) row];
            drawLine (g, area, p, clash.line + ": " + clash.what, describeValue (clash, clash.chosen));
            return;
        }

        const auto& change = view.changes[(size_t) row];
        drawLine (g, area, p, getMarkedName (change), change.what);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (! juce::isPositiveAndBelow (row, view.getNumChanges()))
            return;

        if (view.lines == Lines::merge)
        {
            tickMergeLine (view.context, row, ! view.mergeLines[(size_t) row].ticked);
            return;
        }

        if (view.lines == Lines::clashes)
        {
            view.showClashMenu (row);
            return;
        }

        // A click selects what changed: its track, or the master; the right button shows what can be done with it.
        const auto& change = view.changes[(size_t) row];
        selectChange (view.context, change);

        if (change.track.isValid() && change.kind != VersionChange::Kind::removed)
            view.context.getSongSelection().selectOnly (change.track);

        if (e.mods.isPopupMenu())
            view.showMenu ({ "version.putBack" });
    }

    void selectedRowsChanged (int row) override
    {
        if (view.lines == Lines::changes && juce::isPositiveAndBelow (row, (int) view.changes.size()))
            selectChange (view.context, view.changes[(size_t) row]);
    }

    void returnKeyPressed (int row) override
    {
        if (view.lines == Lines::merge && juce::isPositiveAndBelow (row, (int) view.mergeLines.size()))
            tickMergeLine (view.context, row, ! view.mergeLines[(size_t) row].ticked);
        else if (view.lines == Lines::clashes)
            view.showClashMenu (row);
    }

    juce::String getNameForRow (int row) override { return view.describeChange (row); }

    juce::String getTooltipForRow (int row) override
    {
        if (! juce::isPositiveAndBelow (row, view.getNumChanges()))
            return {};

        if (view.lines == Lines::merge)
            return view.describeChange (row) + ". Click to take it, or to leave it out.";

        if (view.lines == Lines::clashes)
            return view.describeChange (row) + ". Both lines changed it: click to choose whose value the song has.";

        return view.describeChange (row) + ". Click to select it in the song; right-click to put it back.";
    }

private:
    VersionsView& view;
};

//==============================================================================
// The history: "Now", the song as it is, then every branch's versions, newest first, each a dot on its branch's line, in its
// branch's column.
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

        // Its play mark, while any version has a bounce: what is heard in the accent's color.
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
            g.drawText ("Now", area, juce::Justification::centredLeft, true);
            return;
        }

        if (! juce::isPositiveAndBelow (row - 1, (int) view.versions.size()))
            return;

        const auto& version = view.versions[(size_t) row - 1];
        g.setColour (view.getBranchColour (version.branch));
        g.fillEllipse (dotAt (version.branch));

        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        const auto when = describeWhen (version.saved, juce::Time::getCurrentTime());
        g.drawText (when, area.removeFromRight (juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), when)) + Space::xs),
                    juce::Justification::centredRight, false);

        auto text = area;

        if (version.note.isNotEmpty())
        {
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
            return "Now: the song as it is, with the changes since its last version. Right-click to make a branch from it."
                   + juce::String (play);

        return view.describeHistoryRow (row) + ". Right-click for what can be done with it." + play;
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        selectedRowsChanged (row);

        // On its play mark: it is heard (Now's: the song again).
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
                             "version.bounce", "version.deleteBounce" });
        else
            view.showMenu ({ "version.hear", "version.newBranch" });
    }

    void selectedRowsChanged (int row) override
    {
        // Now, or a version, which the commands act on.
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
      changesModel (std::make_unique<ChangesModel> (*this)),
      historyModel (std::make_unique<HistoryModel> (*this)),
      changesList ("Changes since the last version", changesModel.get()),
      historyList ("History", historyModel.get())
{
    setTitle ("Versions");
    const auto& p = getPalette (*this);

    setUpHeading (title, "VERSIONS");
    setUpHeading (changesHeading, {});
    setUpHeading (historyHeading, "HISTORY");
    for (auto* label : { &emptyText, &roomText })
    {
        label->setFont (juce::FontOptions (TextSize::small));
        label->setJustificationType (juce::Justification::topLeft);
        label->setColour (juce::Label::textColourId, p.textMuted);
    }

    roomText.setTitle ("Room the versions take");
    branchButton.setTitle ("Branch");
    branchButton.onClick = [this] { showBranchMenu(); };

    nameField.setTitle ("Version name");
    nameField.setTextToShowWhenEmpty ("Name this version (optional)", p.textMuted);
    noteField.setTitle ("Version note");
    noteField.setTextToShowWhenEmpty ("Note: why, or what to try next", p.textMuted);

    for (auto* field : { &nameField, &noteField })
    {
        field->setFont (juce::FontOptions (TextSize::body));
        field->addListener (this);
    }

    saveButton.setButtonText ("Save Version");
    saveButton.onClick = [this]
    {
        setVersionDraft (context, nameField.getText(), noteField.getText()); // as typed, the last key too
        commands.invoke ("version.save");
    };

    // With a bounce, as the settings keep it: the model's for the open song follows it (refresh()).
    bounceBox.setToggleState (settings.get (Settings::versionBounce), juce::dontSendNotification);
    bounceBox.onClick = [this]
    {
        settings.set (Settings::versionBounce, bounceBox.getToggleState());
        setBounceWithVersion (context, bounceBox.getToggleState());
    };

    doneButton.setButtonText ("Done");
    doneButton.onClick = [this] { commands.invoke ("version.showChanges"); };
    mergeButton.setButtonText ("Merge");
    mergeButton.onClick = [this] { commands.invoke ("version.finishMerge"); };
    cancelButton.setButtonText ("Cancel");
    cancelButton.onClick = [this] { commands.invoke ("version.cancelMerge"); };

    for (auto* list : { &changesList, &historyList })
    {
        list->setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list->setTitle (list->getName());
    }

    changesList.setRowHeight (History::changeRowHeight);
    historyList.setRowHeight (History::versionRowHeight);

    for (auto* c : std::initializer_list<juce::Component*> { &title, &branchButton, &nameField, &noteField, &bounceBox, &saveButton,
                                                             &changesHeading, &changesList, &historyHeading, &historyList })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &emptyText, &doneButton, &mergeButton, &cancelButton, &roomText })
        addChildComponent (c);

    refreshTooltips();
    context.addListener (this);
    refresh();
}

VersionsView::~VersionsView()
{
    context.removeListener (this);
}

void VersionsView::refreshTooltips()
{
    saveButton.setTooltip (withShortcut (commands,
                                         "Saves the project and keeps the song as it is now as a version, named as you typed above, or "
                                         "by what changed",
                                         "version.save"));
    nameField.setTooltip ("The version's name. Left empty, the version is named by what changed");
    noteField.setTooltip ("A note to keep with the version: what it tried, or what to try next");
    changesList.setTooltip ("What the song holds now that its last version didn't, by track. Right-click a line to put it back");
    doneButton.setTooltip (withShortcut (commands, "Lists the changes since the last version again", "version.showChanges"));
    mergeButton.setTooltip (withShortcut (commands, "Takes the lines ticked into the song, as one undo step", "version.finishMerge"));
    cancelButton.setTooltip (withShortcut (commands, "Closes the merge's list, taking nothing", "version.cancelMerge"));
    branchButton.setTooltip ("The branch the song is on: click to switch to another, or to start one");
    historyList.setTooltip ("Every version, newest first, each on its branch's line");
    bounceBox.setTooltip ("Saving a version renders the song into it too, as Export Song does, to hear it later against the song");
    roomText.setTooltip ("What the versions take on disk: nearly all of it is bounces, each deleted on its own (Delete Bounce)");
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

void VersionsView::focusName()
{
    if (nameField.isShowing())
        nameField.grabKeyboardFocus();
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
        return (line.ticked ? "Taken: " : "Left out: ") + describeChangeLine (line.change) + (line.clashes ? ", clashes" : "");
    }

    if (lines == Lines::clashes)
    {
        const auto& clash = clashes[(size_t) row];
        return clash.line + ": " + clash.what + ", " + describeValue (clash, clash.chosen);
    }

    return describeChangeLine (changes[(size_t) row]);
}

juce::String VersionsView::describeHistoryRow (int row) const
{
    if (row == 0)
        return heardVersion.isEmpty() && anyBounce ? "Now, heard" : "Now";

    if (! juce::isPositiveAndBelow (row - 1, (int) versions.size()))
        return {};

    const auto& version = versions[(size_t) row - 1];
    juce::String text = version.name + ", " + describeWhen (version.saved, juce::Time::getCurrentTime());

    if (const auto branch = getBranchName (version.branch); branch.isNotEmpty())
        text << ", on " << branch;

    if (version.hasBounce)
        text << (version.id == heardVersion ? ", heard" : ", with a bounce");

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

void VersionsView::showBranchMenu()
{
    // Each branch, the one the song is on ticked; then a new one from Now.
    juce::PopupMenu menu;

    for (const auto& branch : branches)
    {
        menu.addItem (branch.name, true, branch.id == currentBranch,
                      [this, id = branch.id]
                      {
                          if (id == currentBranch)
                              return;

                          selectBranch (context, id);
                          commands.invoke ("version.switchBranch");
                      });
    }

    if (! branches.empty())
        menu.addSeparator();

    menu.addItem ("New Branch from Now", canMakeBranch (context), false,
                  [this]
                  {
                      selectVersion (context, {});
                      commands.invoke ("version.newBranch");
                  });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (branchButton));
}

void VersionsView::showClashMenu (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) clashes.size()))
        return;

    // Its three values, the one the song has ticked: a choice is heard at once.
    juce::PopupMenu menu;
    const auto& clash = clashes[(size_t) index];

    for (int value : { 1, 2, 0 })
        menu.addItem (describeValue (clash, value), true, clash.chosen == value,
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
    heardVersion = getHeardVersion (context);
    anyBounce = std::any_of (versions.begin(), versions.end(), [] (const VersionInfo& version) { return version.hasBounce; });

    // With a bounce, as the box (the settings) says, for the song open now; and the room the versions take.
    if (getBounceWithVersion (context) != bounceBox.getToggleState())
        setBounceWithVersion (context, bounceBox.getToggleState());

    bounceBox.setEnabled (keeps);
    const auto room = getVersionsRoom (context);
    roomText.setText (versions.empty() ? juce::String()
                                       : "Versions take " + juce::File::descriptionOfSizeInBytes (room.songs) + ", bounces "
                                             + juce::File::descriptionOfSizeInBytes (room.bounces),
                      juce::dontSendNotification);
    roomText.setVisible (roomText.getText().isNotEmpty());

    const auto branch = getCurrentBranch (context);
    currentBranch = branch.has_value() ? branch->id : juce::String();
    refreshHistoryLines (branch.has_value() ? branch->head : juce::String());
    branchButton.setButtonText (branch.has_value() ? branch->name + juce::String::fromUTF8 (" \xe2\x96\xbe") : juce::String());
    branchButton.setColour (juce::TextButton::textColourOffId, branch.has_value() ? branch->colour : getPalette (*this).textMuted);
    branchButton.setVisible (branch.has_value());

    // A merge's list while it shows; its clashes while it waits for its version; otherwise the changes.
    lines = ! mergeLines.empty() ? Lines::merge : ! clashes.empty() ? Lines::clashes : Lines::changes;
    const auto last = getLastVersion (context);
    const auto compared = getComparedVersion (context);
    const auto merged = describeMerge (context);
    juce::String comparedName;

    for (const auto& version : versions)
        if (version.id == compared)
            comparedName = version.name;

    const auto count = "  " + juce::String (getNumChanges());
    const auto branchNameText = branch.has_value() ? branch->name : juce::String();

    changesHeading.setText (lines == Lines::merge     ? "MERGE \"" + merged + "\" INTO " + branchNameText + count
                            : lines == Lines::clashes ? "CLASHES" + count
                            : compared.isNotEmpty()   ? "TAKE FROM \"" + comparedName + "\"" + count
                            : last.has_value()        ? "CHANGES SINCE \"" + last->name + "\"" + count
                                                      : juce::String ("CHANGES"),
                            juce::dontSendNotification);

    doneButton.setVisible (lines == Lines::changes && compared.isNotEmpty());
    mergeButton.setVisible (lines == Lines::merge);
    cancelButton.setVisible (lines == Lines::merge);

    // What there is to say while there is nothing to list, and while a merge waits for its version.
    juce::String said = ! keeps   ? juce::String ("Versions are kept for a project, once it has been saved.")
                        : reading ? juce::String ("Reading the versions...")
                        : ! last  ? juce::String ("No versions yet. Save Version keeps the song as it is now, to come back to.")
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

    // The drafts the model holds: empty again once a version was saved with them.
    if (nameField.getText() != getVersionDraftName (context))
        nameField.setText (getVersionDraftName (context), false);

    if (noteField.getText() != getVersionDraftNote (context))
        noteField.setText (getVersionDraftNote (context), false);

    saveButton.setEnabled (canSaveVersion (context));
    saveButton.setButtonText (isSavingVersion (context) ? "Saving..." : "Save Version");
    nameField.setEnabled (keeps);
    noteField.setEnabled (keeps);

    changesList.updateContent();
    historyList.updateContent();
    changesList.repaint();
    historyList.repaint();
    resized();
}

void VersionsView::refreshHistoryLines (const juce::String& nowFollows)
{
    // Each version's lines to the versions it follows, and Now's to the current branch's last version (or, on a branch without
    // versions yet, the one it was made from).
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

void VersionsView::textEditorTextChanged (juce::TextEditor&)
{
    setVersionDraft (context, nameField.getText(), noteField.getText());
}

void VersionsView::textEditorReturnKeyPressed (juce::TextEditor&)
{
    setVersionDraft (context, nameField.getText(), noteField.getText());
    commands.invoke ("version.save");
}

void VersionsView::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void VersionsView::resized()
{
    auto area = getLocalBounds().reduced (Space::s);
    auto top = area.removeFromTop (controlHeight);
    branchButton.setBounds (top.removeFromRight (top.getWidth() / 2));
    title.setBounds (top);
    area.removeFromTop (Space::xs);
    nameField.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::xs);
    noteField.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::xs);
    auto saveRow = area.removeFromTop (controlHeight);
    saveButton.setBounds (saveRow.removeFromRight (juce::jmin (saveRow.getWidth(), History::saveButtonWidth)));
    saveRow.removeFromRight (Space::xs);
    bounceBox.setBounds (saveRow);
    area.removeFromTop (Space::s);

    // The room the versions take, at the foot of the view.
    if (roomText.isVisible())
        roomText.setBounds (area.removeFromBottom (controlHeight));

    // The lines, as many rows as they have (up to half of what is left), then the history.
    auto heading = area.removeFromTop (controlHeight);

    if (doneButton.isVisible())
        doneButton.setBounds (heading.removeFromRight (juce::jmin (heading.getWidth() / 3, History::buttonWidth)));

    changesHeading.setBounds (heading);

    if (mergeButton.isVisible())
    {
        auto buttons = area.removeFromTop (controlHeight);
        cancelButton.setBounds (buttons.removeFromRight (juce::jmin (buttons.getWidth() / 2, History::buttonWidth)));
        buttons.removeFromRight (Space::xs);
        mergeButton.setBounds (buttons.removeFromRight (juce::jmin (buttons.getWidth(), History::buttonWidth)));
        area.removeFromTop (Space::xs);
    }

    if (emptyText.isVisible())
        emptyText.setBounds (area.removeFromTop (History::noteHeight + (isMerging (context) ? History::noteHeight : 0)));

    const auto rows = getNumChanges();
    const auto listHeight = juce::jmin (area.getHeight() / 2, juce::jmax (1, rows) * History::changeRowHeight);
    changesList.setBounds (rows == 0 ? juce::Rectangle<int>() : area.removeFromTop (listHeight));
    area.removeFromTop (Space::s);
    historyHeading.setBounds (area.removeFromTop (controlHeight));
    historyList.setBounds (area);
}
} // namespace phazite::ui
