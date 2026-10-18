// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProgressionsPanel.h"
#include "model/chords/ChordModel.h"
#include "model/engine/Preview.h"
#include "model/project/ProjectContext.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int rowHeight = 44;
    constexpr int labelWidth = 52;
    constexpr int boxWidth = 160;
    constexpr int aboutHeight = 36;
    constexpr int anyID = 1; // the first item of the mood and genre boxes

    juce::String getTargetName (ProgressionsPanel::Target target)
    {
        switch (target)
        {
            case ProgressionsPanel::Target::selectedSections:
                return "The selected sections";
            case ProgressionsPanel::Target::selectedRange:
                return "The selected time range";
            case ProgressionsPanel::Target::wholeSong:
            default:
                return "The whole song";
        }
    }

    juce::String capitalise (const juce::String& tag)
    {
        return tag.substring (0, 1).toUpperCase() + tag.substring (1);
    }

    void fillTagBox (juce::ComboBox& box, const juce::String& any, const juce::StringArray& tags)
    {
        box.addItem (any, anyID);

        for (int i = 0; i < tags.size(); ++i)
            box.addItem (capitalise (tags[i]), anyID + 1 + i);

        box.setSelectedId (anyID, juce::dontSendNotification);
    }

    juce::String getTag (const juce::ComboBox& box, const juce::StringArray& tags)
    {
        const auto index = box.getSelectedId() - anyID - 1;
        return juce::isPositiveAndBelow (index, tags.size()) ? tags[index] : juce::String();
    }
} // namespace

ProgressionsPanel::ProgressionsPanel (ProjectContext& c, Previewer& p)
    : context (c),
      previewer (p),
      previewFile (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Phazite chord progression.mid"))
{
    setTitle ("Chord Progressions");

    heading.setText ("Chord Progressions", juce::dontSendNotification);
    heading.setFont (juce::FontOptions (TextSize::heading, juce::Font::bold));
    about.setJustificationType (juce::Justification::topLeft);

    moodLabel.setText ("Mood", juce::dontSendNotification);
    genreLabel.setText ("Genre", juce::dontSendNotification);
    targetLabel.setText ("Put on", juce::dontSendNotification);

    moodBox.setTitle ("Mood");
    fillTagBox (moodBox, "Any mood", getProgressionMoods());
    moodBox.onChange = [this] { filterChanged(); };

    genreBox.setTitle ("Genre");
    fillTagBox (genreBox, "Any genre", getProgressionGenres());
    genreBox.onChange = [this] { filterChanged(); };

    targetBox.setTitle ("Where the progression goes");
    targetBox.onChange = [this] { updateButtons(); };

    list.setTitle ("Progressions");
    list.setRowHeight (rowHeight);
    list.setMultipleSelectionEnabled (false);

    playButton.setTooltip ("Plays the selected progression in the song's key and tempo, over and over");
    playButton.onClick = [this] { isPlaying() ? stop() : play(); };

    applyButton.setTooltip ("Lays the selected progression on the chord lane, repeated to fill where it goes");
    applyButton.onClick = [this] { apply(); };

    for (auto* component : std::initializer_list<juce::Component*> { &heading, &about, &moodLabel, &moodBox, &genreLabel, &genreBox, &list,
                                                                     &playButton, &targetLabel, &targetBox, &applyButton })
        addAndMakeVisible (component);

    filterChanged();
    updateTargets();
    startTimerHz (4);
}

ProgressionsPanel::~ProgressionsPanel()
{
    stopTimer();
    stop();
}

//==============================================================================
void ProgressionsPanel::setFilter (const juce::String& mood, const juce::String& genre)
{
    const auto moods = getProgressionMoods();
    const auto genres = getProgressionGenres();
    moodBox.setSelectedId (moods.contains (mood) ? anyID + 1 + moods.indexOf (mood) : anyID, juce::dontSendNotification);
    genreBox.setSelectedId (genres.contains (genre) ? anyID + 1 + genres.indexOf (genre) : anyID, juce::dontSendNotification);
    filterChanged();
}

void ProgressionsPanel::filterChanged()
{
    const auto* selected = getSelected();
    shown = findProgressions (getTag (moodBox, getProgressionMoods()), getTag (genreBox, getProgressionGenres()));
    list.updateContent();
    list.repaint();
    select (selected);
}

void ProgressionsPanel::select (const Progression* progression)
{
    const auto found = std::find (shown.begin(), shown.end(), progression);

    if (progression != nullptr && found != shown.end())
        list.selectRow ((int) (found - shown.begin()));
    else if (! shown.empty())
        list.selectRow (0);
    else
        list.deselectAllRows();

    updateButtons();
}

const Progression* ProgressionsPanel::getSelected() const
{
    const auto row = list.getSelectedRow();
    return juce::isPositiveAndBelow (row, (int) shown.size()) ? shown[(size_t) row] : nullptr;
}

//==============================================================================
std::optional<BeatRange> ProgressionsPanel::getTargetRange (Target target) const
{
    switch (target)
    {
        case Target::selectedSections:
            return getSelectedSectionsRange (context);
        case Target::selectedRange:
            return getSelectedTimeRange (context);
        case Target::wholeSong:
        default:
            return getWholeSongRange (context);
    }
}

std::vector<ProgressionsPanel::Target> ProgressionsPanel::getTargets() const
{
    std::vector<Target> available;

    for (const auto target : { Target::selectedSections, Target::selectedRange, Target::wholeSong })
        if (target == Target::wholeSong || getTargetRange (target).has_value())
            available.push_back (target);

    return available;
}

void ProgressionsPanel::updateTargets()
{
    // What is selected now; the choice stays if it still can be made.
    const auto index = targetBox.getSelectedItemIndex();
    const auto previous =
        juce::isPositiveAndBelow (index, (int) targets.size()) ? std::optional<Target> (targets[(size_t) index]) : std::nullopt;

    targets = getTargets();
    targetBox.clear (juce::dontSendNotification);

    for (size_t i = 0; i < targets.size(); ++i)
        targetBox.addItem (getTargetName (targets[i]), (int) i + 1);

    const auto kept = previous.has_value() ? std::find (targets.begin(), targets.end(), *previous) : targets.end();
    targetBox.setSelectedItemIndex (kept != targets.end() ? (int) (kept - targets.begin()) : 0, juce::dontSendNotification);
    updateButtons();
}

void ProgressionsPanel::setTarget (Target target)
{
    updateTargets();

    if (const auto found = std::find (targets.begin(), targets.end(), target); found != targets.end())
        targetBox.setSelectedItemIndex ((int) (found - targets.begin()), juce::dontSendNotification);

    updateButtons();
}

BeatRange ProgressionsPanel::getRangeFor (const Progression& progression) const
{
    const auto index = targetBox.getSelectedItemIndex();
    const auto target = juce::isPositiveAndBelow (index, (int) targets.size()) ? targets[(size_t) index] : Target::wholeSong;

    if (const auto range = getTargetRange (target))
        return *range;

    // An empty song: once from its start.
    return { BeatPosition(), getProgressionLength (context, progression, {}) };
}

Key ProgressionsPanel::getKey() const
{
    const auto* progression = getSelected();
    return getLaneKeyAt (context, progression != nullptr ? getRangeFor (*progression).getStart() : BeatPosition());
}

void ProgressionsPanel::apply()
{
    const auto* progression = getSelected();

    if (progression == nullptr || ! context.hasProject())
        return;

    applyProgression (context, *progression, getRangeFor (*progression));
}

//==============================================================================
void ProgressionsPanel::play()
{
    const auto* progression = getSelected();

    if (progression == nullptr || ! context.hasProject())
        return;

    if (writeProgressionPreview (context, *progression, getRangeFor (*progression).getStart(), previewFile))
        previewer.play (previewFile, context);

    updateButtons();
}

void ProgressionsPanel::stop()
{
    if (isPlaying())
        previewer.stop();

    updateButtons();
}

bool ProgressionsPanel::isPlaying() const
{
    return previewer.isPlaying() && previewer.getFile() == previewFile;
}

void ProgressionsPanel::updateButtons()
{
    const auto canUse = getSelected() != nullptr && context.hasProject();
    playButton.setButtonText (isPlaying() ? "Stop" : "Play");
    playButton.setEnabled (canUse || isPlaying());
    applyButton.setEnabled (canUse);

    about.setText ("Play one to hear it in the song's key (" + describeKey (getKey())
                       + ") and tempo, then put it on the chord lane. Change the key there, and the chords follow.",
                   juce::dontSendNotification);
    list.repaint(); // the chords' names, in the key where it would go
}

void ProgressionsPanel::timerCallback()
{
    // The selection in the song, and the preview stopping elsewhere (the browser plays another sound).
    if (getTargets() != targets)
        updateTargets();

    if (playButton.getButtonText() != (isPlaying() ? "Stop" : "Play"))
        updateButtons();
}

void ProgressionsPanel::visibilityChanged()
{
    if (isVisible())
        updateTargets();
}

//==============================================================================
int ProgressionsPanel::getNumRows()
{
    return (int) shown.size();
}

void ProgressionsPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) shown.size()))
        return;

    const auto& p = getPalette (*this);
    const auto& progression = *shown[(size_t) row];

    if (selected)
        g.fillAll (p.accent.withAlpha (selectionWash));

    auto area = juce::Rectangle<int> (width, height).reduced (Space::s, Space::xs);
    auto top = area.removeFromTop (area.getHeight() / 2);

    // The name and the chords in the key; under them, the numerals and the feel.
    const juce::Font nameFont { juce::FontOptions (TextSize::body, juce::Font::bold) };
    g.setColour (p.text);
    g.setFont (nameFont);
    g.drawText (progression.name, top, juce::Justification::centredLeft, true);

    const auto nameWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (nameFont, progression.name)) + Space::m;
    g.setFont (juce::FontOptions (TextSize::body));
    g.drawText (getChordNamesIn (progression, getKey()).joinIntoString (" - "), top.withTrimmedLeft (nameWidth),
                juce::Justification::centredLeft, true);

    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (progression.describeNumerals() + "    " + progression.feel, area, juce::Justification::centredLeft, true);
}

void ProgressionsPanel::selectedRowsChanged (int)
{
    // A progression playing goes on playing the one selected.
    const auto playing = isPlaying();
    updateButtons();

    if (playing)
        play();
}

void ProgressionsPanel::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    list.selectRow (row);
    play();
}

juce::String ProgressionsPanel::getNameForRow (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) shown.size()))
        return {};

    const auto& progression = *shown[(size_t) row];
    return progression.name + ": " + getChordNamesIn (progression, getKey()).joinIntoString (", ") + ". " + progression.feel;
}

//==============================================================================
void ProgressionsPanel::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void ProgressionsPanel::resized()
{
    auto area = getLocalBounds().reduced (Space::xl);
    heading.setBounds (area.removeFromTop (32));
    about.setBounds (area.removeFromTop (aboutHeight));
    area.removeFromTop (Space::s);

    auto filters = area.removeFromTop (controlHeight);
    moodLabel.setBounds (filters.removeFromLeft (labelWidth));
    moodBox.setBounds (filters.removeFromLeft (boxWidth));
    filters.removeFromLeft (Space::l);
    genreLabel.setBounds (filters.removeFromLeft (labelWidth));
    genreBox.setBounds (filters.removeFromLeft (boxWidth));
    area.removeFromTop (Space::s);

    auto buttons = area.removeFromBottom (controlHeight);
    area.removeFromBottom (Space::m);
    list.setBounds (area);

    playButton.setBounds (buttons.removeFromLeft (88));
    applyButton.setBounds (buttons.removeFromRight (176));
    buttons.removeFromRight (Space::s);
    targetBox.setBounds (buttons.removeFromRight (196));
    targetLabel.setBounds (buttons.removeFromRight (labelWidth));
}
} // namespace phazite::ui
