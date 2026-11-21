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
      finder (c),
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
    list.setTooltip ("Files in the project's Audio and Generated folders that neither the song, nor its file as saved, nor a "
                     "backup, nor a version, nor a branch plays. Click one to keep it");
    deleteButton.setTooltip ("Moves the ticked files to the recycle bin");
    deleteButton.onClick = [this] { deleteTicked(); };
    cancelButton.onClick = [this] { close(); };

    for (auto* component : std::initializer_list<juce::Component*> { &title, &summary, &list, &deleteButton, &cancelButton })
        addAndMakeVisible (component);

    finder.onFound = [this] { showFound(); };
    finder.find();
    refreshSummary();
    setSize (width, height);
}

void CleanUpPanel::showFound()
{
    // What the look found, or nothing while it can't say which are unused.
    const auto& found = finder.getFound();
    files = found.has_value() && found->unreadable.isEmpty() ? found->files : std::vector<UnusedAudioFile>();
    ticked.assign (files.size(), true);
    list.updateContent();
    list.repaint();
    refreshSummary();
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

void CleanUpPanel::refreshSummary()
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

    // While it looks, or why it can't; what it found; and that what a version or an automatic save still plays is kept, which a
    // file once saved in a version always is (model/project/UnusedAudio.h).
    const auto& found = finder.getFound();
    const auto why = getWhyNotCleanUpAudio (context);
    const juce::String kept ("Files a version or an automatic save still plays are kept.");
    juce::String said;

    if (finder.isDeleting())
        said = "Deleting " + juce::String (count) + (count == 1 ? " file..." : " files...");
    else if (found.has_value() && ! found->unreadable.isEmpty())
        said = "Phazite can't tell which audio files are unused now: " + found->unreadable.joinIntoString ("; ") + ". " + kept;
    else if (finder.isLooking() || (! found.has_value() && why.isEmpty()))
        said = "Looking for the audio files nothing plays...";
    else if (! found.has_value())
        said = why;
    else if (files.empty())
        said = "No unused audio: the song, its saved files, its versions or its branches play every file in the project's Audio "
               "and Generated folders. "
               + kept;
    else
        said = juce::String (count) + " of " + juce::String ((int) files.size()) + " ticked, taking "
               + juce::File::descriptionOfSizeInBytes (size) + ". " + kept
               + " Undo doesn't bring deleted files back; the recycle bin keeps them.";

    summary.setText (done.isNotEmpty() ? done + " " + said : said, juce::dontSendNotification);
    deleteButton.setEnabled (count > 0 && ! finder.isLooking() && ! finder.isDeleting() && why.isEmpty());
}

void CleanUpPanel::deleteTicked()
{
    juce::Array<juce::File> toDelete;

    for (size_t i = 0; i < files.size(); ++i)
        if (ticked[i])
            toDelete.add (files[i].file);

    // On the finder's thread, the window held meanwhile (model/project/UnusedAudio.h); said here once done.
    finder.deleteUnused (context, toDelete, removal, [this] (UnusedAudioFinder::Deleted deleted) { showDeleted (deleted); });
    refreshSummary();
}

void CleanUpPanel::showDeleted (const UnusedAudioFinder::Deleted& deleted)
{
    const auto counted = juce::String (deleted.count) + (deleted.count == 1 ? " file" : " files");
    const auto moved = removal == Removal::toRecycleBin ? "Moved " + counted + " to the recycle bin." : "Deleted " + counted + ".";
    done = deleted.count == 0 && deleted.refused.isNotEmpty() ? deleted.refused
           : deleted.refused.isNotEmpty()                     ? moved + " " + deleted.refused
                                                              : moved;
    cancelButton.setButtonText ("Close");
    files.clear();
    ticked.clear();
    list.updateContent();
    finder.find(); // what is left, shown once found
    refreshSummary();
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
