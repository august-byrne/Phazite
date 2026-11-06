// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CleanUpPanel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int buttonWidth = 96; // as the export window's
} // namespace

CleanUpPanel::CleanUpPanel (ProjectContext& c, Removal r)
    : context (c),
      removal (r),
      list ("Unused audio files", this)
{
    setTitle ("Clean Up Unused Audio");
    title.setText ("Audio files no song of this project plays", juce::dontSendNotification);
    title.setFont (juce::FontOptions (TextSize::heading, juce::Font::bold));
    summary.setFont (juce::FontOptions (TextSize::body));
    summary.setJustificationType (juce::Justification::topLeft);
    setThemeColour (summary, juce::Label::textColourId, &Palette::textMuted);
    list.setTitle (list.getName());
    list.setRowHeight (controlHeight);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setTooltip ("Files in the project's Audio and Generated folders that neither the song, nor a version, nor a branch "
                     "plays. Click one to keep it");
    deleteButton.setTooltip ("Moves the ticked files to the recycle bin");
    deleteButton.onClick = [this] { deleteTicked(); };
    cancelButton.onClick = [this] { close(); };

    for (auto* component : std::initializer_list<juce::Component*> { &title, &summary, &list, &deleteButton, &cancelButton })
        addAndMakeVisible (component);

    findFiles();
    refreshSummary();
    setSize (width, height);
}

void CleanUpPanel::findFiles()
{
    files = findUnusedAudio (context);
    ticked.assign (files.size(), true);
    list.updateContent();
    list.repaint();
}

juce::String CleanUpPanel::describeFile (int row) const
{
    if (! juce::isPositiveAndBelow (row, getNumFiles()))
        return {};

    const auto& file = files[(size_t) row];
    return file.name + ", " + juce::File::descriptionOfSizeInBytes (file.size) + (ticked[(size_t) row] ? ", to delete" : ", kept");
}

bool CleanUpPanel::isTicked (int row) const
{
    return juce::isPositiveAndBelow (row, getNumFiles()) && ticked[(size_t) row];
}

void CleanUpPanel::setTicked (int row, bool tick)
{
    if (! juce::isPositiveAndBelow (row, getNumFiles()))
        return;

    ticked[(size_t) row] = tick;
    list.repaintRow (row);
    refreshSummary();
}

void CleanUpPanel::refreshSummary (const juce::String& done)
{
    juce::int64 size = 0;
    auto count = 0;

    for (size_t i = 0; i < files.size(); ++i)
    {
        if (ticked[i])
        {
            size += files[i].size;
            ++count;
        }
    }

    const auto said = files.empty() ? juce::String ("No unused audio: the song, its versions or its branches play every file in the "
                                                    "project's Audio and Generated folders.")
                                    : juce::String (count) + " of " + juce::String ((int) files.size()) + " ticked, taking "
                                          + juce::File::descriptionOfSizeInBytes (size)
                                          + ". Undo doesn't bring deleted files back; the recycle bin keeps them.";
    summary.setText (done.isNotEmpty() ? done + " " + said : said, juce::dontSendNotification);
    deleteButton.setEnabled (count > 0);
}

void CleanUpPanel::deleteTicked()
{
    juce::Array<juce::File> toDelete;

    for (size_t i = 0; i < files.size(); ++i)
        if (ticked[i])
            toDelete.add (files[i].file);

    const auto deleted = deleteUnusedAudio (context, toDelete, removal);
    const auto counted = juce::String (deleted) + (deleted == 1 ? " file" : " files");
    findFiles();
    refreshSummary (removal == Removal::toRecycleBin ? "Moved " + counted + " to the recycle bin." : "Deleted " + counted + ".");
    cancelButton.setButtonText ("Close");
}

void CleanUpPanel::close()
{
    if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
        window->exitModalState (0); // which deletes it
}

void CleanUpPanel::paintListBoxItem (int row, juce::Graphics& g, int rowWidth, int rowHeight, bool selected)
{
    if (! juce::isPositiveAndBelow (row, getNumFiles()))
        return;

    const auto& p = getPalette (*this);
    auto area = juce::Rectangle<int> (rowWidth, rowHeight);

    if (selected)
    {
        g.setColour (p.accent.withAlpha (selectionWash));
        g.fillRect (area);
    }

    area.reduce (Space::s, 0);
    const auto box = area.removeFromLeft (rowHeight);
    getLookAndFeel().drawTickBox (g, *this, (float) box.getX(), (float) box.getY() + (float) Space::xs, (float) box.getWidth() - Space::s,
                                  (float) box.getHeight() - Space::s, ticked[(size_t) row], true, false, false);

    const auto& file = files[(size_t) row];
    const auto size = juce::File::descriptionOfSizeInBytes (file.size);
    g.setFont (juce::FontOptions (TextSize::small));
    g.setColour (p.textMuted);
    g.drawText (size, area.removeFromRight (juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), size)) + Space::s),
                juce::Justification::centredRight, false);
    g.setFont (juce::FontOptions (TextSize::body));
    g.setColour (p.text);
    g.drawText (file.name, area, juce::Justification::centredLeft, true);
}

void CleanUpPanel::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    setTicked (row, ! isTicked (row));
}

void CleanUpPanel::returnKeyPressed (int row)
{
    setTicked (row, ! isTicked (row));
}

void CleanUpPanel::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void CleanUpPanel::resized()
{
    auto area = getLocalBounds().reduced (Space::xl);
    title.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::s);
    auto buttons = area.removeFromBottom (controlHeight);
    cancelButton.setBounds (buttons.removeFromRight (buttonWidth));
    buttons.removeFromRight (Space::s);
    deleteButton.setBounds (buttons.removeFromRight (buttonWidth));
    area.removeFromBottom (Space::m);
    summary.setBounds (area.removeFromBottom (controlHeight * 2));
    area.removeFromBottom (Space::s);
    list.setBounds (area);
}
} // namespace phazite::ui
