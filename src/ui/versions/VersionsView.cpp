// SPDX-License-Identifier: AGPL-3.0-or-later
#include "VersionsView.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    juce::String describeChangeLine (const VersionChange& change)
    {
        const auto name = (change.kind == VersionChange::Kind::added     ? "+ "
                           : change.kind == VersionChange::Kind::removed ? "- "
                                                                         : "")
                          + change.name;
        return change.what.isEmpty() ? name : name + ": " + change.what;
    }

    void setUpHeading (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
        label.setJustificationType (juce::Justification::centredLeft);
        label.setMinimumHorizontalScale (1.0f);
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
// The changes since the last version: a track's name, then what changed of it; a click selects the track.
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
        if (! juce::isPositiveAndBelow (row, (int) view.changes.size()))
            return;

        const auto& p = getPalette (view);
        const auto& change = view.changes[(size_t) row];
        auto area = juce::Rectangle<int> (width, height).reduced (Space::s, 0);

        if (selected)
        {
            g.setColour (p.accent.withAlpha (selectionWash));
            g.fillRect (0, 0, width, height);
        }

        const auto name = (change.kind == VersionChange::Kind::added     ? "+ "
                           : change.kind == VersionChange::Kind::removed ? "- "
                                                                         : "")
                          + change.name;
        g.setColour (p.text);
        g.setFont (juce::FontOptions (TextSize::body));
        const auto nameWidth =
            juce::jmin (area.getWidth() / 2, juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), name)) + Space::m);
        g.drawText (name, area.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);

        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (change.what, area, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (! juce::isPositiveAndBelow (row, (int) view.changes.size()))
            return;

        // A click selects what changed: its track, or the master.
        if (const auto& change = view.changes[(size_t) row]; change.track.isValid() && change.kind != VersionChange::Kind::removed)
            view.context.getSongSelection().selectOnly (change.track);
    }

    juce::String getNameForRow (int row) override { return view.describeChange (row); }

    juce::String getTooltipForRow (int row) override
    {
        if (! juce::isPositiveAndBelow (row, (int) view.changes.size()))
            return {};

        return describeChangeLine (view.changes[(size_t) row]) + ". Click to select it in the song.";
    }

private:
    VersionsView& view;
};

//==============================================================================
// The history: "Now", the song as it is, then every branch's versions, newest first, each a dot on its branch's line.
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
        auto lane = area.removeFromLeft (History::laneWidth + Space::xs);
        const auto dot = juce::Rectangle<float> (History::dotSize, History::dotSize)
                             .withCentre ({ (float) lane.getX() + History::laneWidth / 2.0f, (float) height / 2.0f });

        if (row == 0)
        {
            g.setColour (p.textMuted);
            g.drawEllipse (dot.reduced (0.5f), History::lineWidth / 2.0f);
            g.setColour (p.text);
            g.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
            g.drawText ("Now", area, juce::Justification::centredLeft, true);
            return;
        }

        if (! juce::isPositiveAndBelow (row - 1, (int) view.versions.size()))
            return;

        const auto& version = view.versions[(size_t) row - 1];
        const auto colour = view.getBranchColour (version.branch);

        // Its branch's line through it, to the version before it.
        g.setColour (colour);
        g.fillRect (juce::Rectangle<float> (History::lineWidth, (float) height).withCentre (dot.getCentre()));
        g.fillEllipse (dot);

        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        const auto when = describeWhen (version.saved, juce::Time::getCurrentTime());
        g.drawText (when, area.removeFromRight (juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), when)) + Space::xs),
                    juce::Justification::centredRight, false);

        auto lines = area;

        if (version.note.isNotEmpty())
        {
            g.drawText (version.note, lines.removeFromBottom (height / 2 - 1), juce::Justification::centredLeft, true);
            lines = lines.withTrimmedTop (2);
        }

        g.setColour (version.hasOwnName ? p.text : p.textMuted);
        g.setFont (juce::FontOptions (TextSize::body));
        g.drawText (version.name, lines, juce::Justification::centredLeft, true);
    }

    juce::String getNameForRow (int row) override { return view.describeHistoryRow (row); }

    juce::String getTooltipForRow (int row) override
    {
        if (row == 0)
            return "Now: the song as it is, with the changes since its last version";

        return view.describeHistoryRow (row);
    }

private:
    VersionsView& view;
};

//==============================================================================
VersionsView::VersionsView (CommandBridge& bridge, ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext),
      changesModel (std::make_unique<ChangesModel> (*this)),
      historyModel (std::make_unique<HistoryModel> (*this)),
      changesList ("Changes since the last version", changesModel.get()),
      historyList ("History", historyModel.get())
{
    setTitle ("Versions");
    const auto& p = getPalette (*this);

    setUpHeading (title, "VERSIONS");
    branchName.setFont (juce::FontOptions (TextSize::small));
    branchName.setJustificationType (juce::Justification::centredRight);
    branchName.setTitle ("Branch");
    setUpHeading (changesHeading, {});
    setUpHeading (historyHeading, "HISTORY");
    emptyText.setFont (juce::FontOptions (TextSize::small));
    emptyText.setJustificationType (juce::Justification::topLeft);
    emptyText.setColour (juce::Label::textColourId, p.textMuted);

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

    for (auto* list : { &changesList, &historyList })
    {
        list->setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list->setTitle (list->getName());
    }

    changesList.setRowHeight (History::changeRowHeight);
    historyList.setRowHeight (History::versionRowHeight);

    for (auto* c : std::initializer_list<juce::Component*> { &title, &branchName, &nameField, &noteField, &saveButton, &changesHeading,
                                                             &changesList, &historyHeading, &historyList })
        addAndMakeVisible (c);

    addChildComponent (emptyText);
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
    changesList.setTooltip ("What the song holds now that its last version didn't, by track");
    historyList.setTooltip ("Every version, newest first, each on its branch's line");
}

void VersionsView::focusName()
{
    if (nameField.isShowing())
        nameField.grabKeyboardFocus();
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

juce::String VersionsView::describeChange (int row) const
{
    return juce::isPositiveAndBelow (row, (int) changes.size()) ? describeChangeLine (changes[(size_t) row]) : juce::String();
}

juce::String VersionsView::describeHistoryRow (int row) const
{
    if (row == 0)
        return "Now";

    if (! juce::isPositiveAndBelow (row - 1, (int) versions.size()))
        return {};

    const auto& version = versions[(size_t) row - 1];
    juce::String text = version.name + ", " + describeWhen (version.saved, juce::Time::getCurrentTime());

    if (const auto branch = getBranchName (version.branch); branch.isNotEmpty())
        text << ", on " << branch;

    if (version.note.isNotEmpty())
        text << ". " << version.note;

    return text;
}

void VersionsView::refresh()
{
    const auto keeps = keepsVersions (context);
    const auto reading = isReadingVersions (context);
    changes = getChangesSinceLastVersion (context);
    versions = getVersions (context);
    branches = getBranches (context);

    const auto branch = getCurrentBranch (context);
    branchName.setText (branch.has_value() ? branch->name : juce::String(), juce::dontSendNotification);
    branchName.setColour (juce::Label::textColourId, branch.has_value() ? branch->colour : getPalette (*this).textMuted);

    const auto last = getLastVersion (context);
    changesHeading.setText (last.has_value() ? "CHANGES SINCE \"" + last->name + "\"  " + juce::String ((int) changes.size())
                                             : juce::String ("CHANGES"),
                            juce::dontSendNotification);

    // What there is to say while there is nothing to list.
    emptyText.setText (! keeps           ? juce::String ("Versions are kept for a project, once it has been saved.")
                       : reading         ? juce::String ("Reading the versions...")
                       : ! last          ? juce::String ("No versions yet. Save Version keeps the song as it is now, to come back to.")
                       : changes.empty() ? juce::String ("No changes since the last version.")
                                         : juce::String(),
                       juce::dontSendNotification);
    emptyText.setVisible (emptyText.getText().isNotEmpty());

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
    branchName.setBounds (top.removeFromRight (top.getWidth() / 2));
    title.setBounds (top);
    area.removeFromTop (Space::xs);
    nameField.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::xs);
    noteField.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::xs);
    saveButton.setBounds (area.removeFromTop (controlHeight).removeFromRight (juce::jmin (area.getWidth(), 120)));
    area.removeFromTop (Space::s);

    // The changes, as many rows as they have (up to half of what is left), then the history.
    changesHeading.setBounds (area.removeFromTop (controlHeight));
    const auto changesHeight = juce::jmin (area.getHeight() / 2, juce::jmax (1, (int) changes.size()) * History::changeRowHeight);

    if (emptyText.isVisible())
        emptyText.setBounds (area.removeFromTop (History::noteHeight));

    changesList.setBounds (changes.empty() ? juce::Rectangle<int>() : area.removeFromTop (changesHeight));
    area.removeFromTop (Space::s);
    historyHeading.setBounds (area.removeFromTop (controlHeight));
    historyList.setBounds (area);
}
} // namespace phazite::ui
