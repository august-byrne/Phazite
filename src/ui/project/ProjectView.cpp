// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectView.h"
#include "model/arrangement/SongSelection.h"
#include "model/project/UnusedAudio.h"
#include "ui/input/CommandUI.h"
#include "ui/mixer/DeviceSlots.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <utility>

namespace phazite::ui
{
namespace
{
    constexpr int readsPerSecond = 2;
    constexpr int ticksBetweenReads = 5 * readsPerSecond; // files come and go on the disk: they are looked at again so often
    constexpr juce::uint32 typingPause = 1500;            // milliseconds without a key that end a burst of typing
    constexpr int fewestNoteLines = 3, mostNoteLines = 12;
    const juce::String audioFilePattern ("*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

    // A row's parts in its list, which is as wide as the panel (SideBar): its icon where a first level's chevron is, its text
    // after it, and at its end, inside the row's inset, the column of its action, for a row that has one.
    juce::Rectangle<int> takeIcon (juce::Rectangle<int>& row)
    {
        row.removeFromLeft (SideBar::rowChevron);
        const auto icon = row.removeFromLeft (SideBar::chevronSize);
        row.removeFromLeft (SideBar::gap);
        return icon;
    }

    juce::Rectangle<int> takeActionColumn (juce::Rectangle<int>& row)
    {
        row.removeFromRight (Space::s);
        return row.removeFromRight (SideBar::rowActionWidth);
    }

    // A row's name and what it says of it, after it in the muted colour; and a word at its end (what is wrong), coloured.
    void drawText (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour nameColour, juce::Colour detailColour,
                   const juce::String& name, const juce::String& detail)
    {
        g.setColour (nameColour);
        g.setFont (juce::FontOptions (TextSize::body));
        const auto nameWidth =
            juce::jmin (area.getWidth(), juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), name)) + Space::s);
        g.drawText (name, area.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);
        g.setColour (detailColour);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (detail, area, juce::Justification::centredLeft, true);
    }

    void drawWord (juce::Graphics& g, juce::Rectangle<int>& area, const juce::String& word, juce::Colour colour)
    {
        g.setColour (colour);
        g.setFont (juce::FontOptions (TextSize::small));
        const auto width = juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), word)) + Space::xs;
        g.drawText (word, area.removeFromRight (width), juce::Justification::centredRight, false);
        area.removeFromRight (Space::xs);
    }

    juce::String describeState (PluginState state)
    {
        switch (state)
        {
            case PluginState::crashed:
                return "crashed";
            case PluginState::missing:
                return "not installed";
            case PluginState::off:
                return "off";
            case PluginState::playing:
            default:
                return {};
        }
    }

    // What a header says is wrong, after its name: "2 missing"; "1 crashed, 1 not installed".
    juce::String describeProblems (const std::vector<ProjectPlugin>& plugins)
    {
        juce::StringArray problems;

        for (const auto state : { PluginState::crashed, PluginState::missing, PluginState::off })
            if (const auto count = std::count_if (plugins.begin(), plugins.end(), [state] (const auto& p) { return p.state == state; });
                count > 0)
                problems.add (juce::String ((int) count) + " " + describeState (state));

        return problems.joinIntoString (", ");
    }
} // namespace

//==============================================================================
// Hears every press in the app while a burst of typing goes on: one anywhere but in the notes ends the burst.
struct ProjectView::PressElsewhere : public juce::MouseListener
{
    explicit PressElsewhere (ProjectView& v)
        : view (v)
    {
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto& notes = view.getNotesField();

        if (e.originalComponent != &notes && ! notes.isParentOf (e.originalComponent))
            view.endTyping();
    }

    ProjectView& view;
};

//==============================================================================
// The notes: Enter starts a new line; the keys going elsewhere end the burst of typing.
class ProjectView::NotesField : public juce::TextEditor
{
public:
    explicit NotesField (ProjectView& v)
        : view (v)
    {
    }

    void focusLost (FocusChangeType cause) override
    {
        juce::TextEditor::focusLost (cause);
        view.endTyping();
    }

private:
    ProjectView& view;
};

//==============================================================================
// The audio files: each with its icon, its name and the tracks that play it; a missing one in the warning colour, so said
// at its end, with its Locate... under the pointer or selected.
class ProjectView::AudioModel : public juce::ListBoxModel
{
public:
    explicit AudioModel (ProjectView& v)
        : view (v)
    {
    }

    int getNumRows() override { return view.getNumAudioFiles(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;

        const auto& p = getPalette (view);
        const auto& file = view.audioFiles[(size_t) row];
        const auto textColour =
            drawListRow (g, view, juce::Rectangle<int> (width, height).toFloat(), selected, row == view.audioHover->row);
        auto area = juce::Rectangle<int> (width, height);
        view.soundIcon.draw (g, takeIcon (area).toFloat().withSizeKeepingCentre (History::rowIconSize, History::rowIconSize),
                             file.missing ? p.warning : p.textMuted);

        if (file.missing)
        {
            const auto action = takeActionColumn (area);

            if (view.showsAction (view.audioList, *view.audioHover, row))
                view.locateIcon.draw (g, action.toFloat().withSizeKeepingCentre (History::rowIconSize, History::rowIconSize), p.text);

            drawWord (g, area, "missing", p.warning);
        }
        else
        {
            area.removeFromRight (Space::s);
        }

        drawText (g, area, file.missing ? p.warning : textColour, p.textMuted, file.file.getFileName(),
                  file.channels.joinIntoString (", "));
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && view.getLocatePosition (row).contains (e.getEventRelativeTo (&view.audioList).getPosition()))
            view.locate (row);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { view.locate (row); }
    void returnKeyPressed (int row) override { view.locate (row); }
    void selectedRowsChanged (int) override { view.audioList.repaint(); } // the action on the row selected now

    juce::String getNameForRow (int row) override { return view.describeAudioFile (row); }

    juce::String getTooltipForRow (int row) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return {};

        const auto& file = view.audioFiles[(size_t) row];
        const auto where = file.file.getFullPathName().isNotEmpty() ? file.file.getFullPathName() : file.path;

        if (file.missing)
            return where + ": missing. Locate... (or a double-click) puts a file in its place, in every clip and sound that plays it";

        return where + ": played by " + file.channels.joinIntoString (", ");
    }

private:
    ProjectView& view;
};

//==============================================================================
// The plug-ins: each with its icon (an instrument's or an effect's), its name and its channel; what is wrong with it at its
// end, a crashed one's Reload under the pointer or selected. A click selects it and shows it.
class ProjectView::PluginModel : public juce::ListBoxModel
{
public:
    explicit PluginModel (ProjectView& v)
        : view (v)
    {
    }

    int getNumRows() override { return view.getNumPlugins(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;

        const auto& p = getPalette (view);
        const auto& plugin = view.plugins[(size_t) row];
        const auto textColour =
            drawListRow (g, view, juce::Rectangle<int> (width, height).toFloat(), selected, row == view.pluginHover->row);
        auto area = juce::Rectangle<int> (width, height);
        auto& icon = plugin.isInstrument ? view.instrumentIcon : view.effectIcon;
        icon.draw (g, takeIcon (area).toFloat().withSizeKeepingCentre (History::rowIconSize, History::rowIconSize), p.textMuted);

        if (plugin.state == PluginState::crashed)
        {
            const auto action = takeActionColumn (area);

            if (view.showsAction (view.pluginList, *view.pluginHover, row))
                view.reloadIcon.draw (g, action.toFloat().withSizeKeepingCentre (History::rowIconSize, History::rowIconSize), p.text);
        }
        else
        {
            area.removeFromRight (Space::s);
        }

        if (plugin.state != PluginState::playing)
            drawWord (g, area, describeState (plugin.state),
                      plugin.state == PluginState::crashed   ? p.danger
                      : plugin.state == PluginState::missing ? p.warning
                                                             : p.textMuted);

        drawText (g, area, textColour, p.textMuted, plugin.name, plugin.channelName);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        if (view.getReloadPosition (row).contains (e.getEventRelativeTo (&view.pluginList).getPosition()))
            view.reload (row);
        else
            view.showPlugin (row);
    }

    void returnKeyPressed (int row) override { view.showPlugin (row); }
    void selectedRowsChanged (int) override { view.pluginList.repaint(); }

    juce::String getNameForRow (int row) override { return view.describePlugin (row); }

    juce::String getTooltipForRow (int row) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return {};

        const auto& plugin = view.plugins[(size_t) row];
        const auto who = plugin.name + (plugin.maker.isNotEmpty() ? " by " + plugin.maker : juce::String()) + " (" + plugin.format
                         + "), on " + plugin.channelName;

        switch (plugin.state)
        {
            case PluginState::crashed:
                return who + ": crashed, and plays nothing until it is reloaded. Its arrow reloads it";
            case PluginState::missing:
                return who + ": not installed here, or not found yet. Scan for Plug-ins, in the gear's menu, looks again";
            case PluginState::off:
                return who
                       + ": off, as the project was opened after Phazite closed unexpectedly. Open it again to turn its "
                         "plug-ins on";
            case PluginState::playing:
            default:
                return who + ". Click to show it";
        }
    }

private:
    ProjectView& view;
};

//==============================================================================
ProjectView::ProjectView (CommandBridge& bridge, ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext),
      notesField (std::make_unique<NotesField> (*this)),
      audioModel (std::make_unique<AudioModel> (*this)),
      pluginModel (std::make_unique<PluginModel> (*this)),
      audioList ("Audio Files", audioModel.get()),
      pluginList ("Plug-ins", pluginModel.get()),
      pressElsewhere (std::make_unique<PressElsewhere> (*this))
{
    setTitle ("Project");
    setWantsKeyboardFocus (true); // its icon gives it the keys (and a screen reader its name), and Tab goes on into its parts
    const auto& p = getPalette (*this);

    // The first pane has no line above it, as in VS Code; each folds under its header.
    notesSection.setLineAbove (false);

    for (auto* section : { &notesSection, &audioSection, &pluginSection })
    {
        section->onClick = [this] { resized(); };
        addAndMakeVisible (section);
    }

    notesSection.set ("Notes", {}, 0);
    audioSection.set ("Audio Files", {}, 0);
    pluginSection.set ("Plug-ins", {}, 0);
    notesField->setTitle ("Notes");
    notesField->setMultiLine (true, true);
    notesField->setReturnKeyStartsNewLine (true);
    notesField->setScrollbarsShown (true);
    notesField->setFont (juce::FontOptions (TextSize::body));
    notesField->setTextToShowWhenEmpty ("Notes about the song: ideas, lyrics, what is left to do", p.textMuted);
    notesField->onTextChange = [this] { notesChanged(); };
    addAndMakeVisible (*notesField);

    for (auto* list : { &audioList, &pluginList })
    {
        list->setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list->setTitle (list->getName());
        list->setRowHeight (ListRow::height);
        addAndMakeVisible (list);
    }

    const auto repaintRows = [] (juce::ListBox& list)
    {
        return [&list] (int previous, int row)
        {
            list.repaintRow (previous);
            list.repaintRow (row);
        };
    };

    audioHover = std::make_unique<ListRowHover> (audioList, repaintRows (audioList));
    pluginHover = std::make_unique<ListRowHover> (pluginList, repaintRows (pluginList));

    for (auto* label : { &emptyAudio, &emptyPlugins, &unusedText })
    {
        label->setFont (juce::FontOptions (TextSize::small));
        label->setJustificationType (juce::Justification::topLeft);
        setThemeColour (*label, juce::Label::textColourId, &Palette::textMuted);
        addChildComponent (label);
    }

    emptyAudio.setText ("No audio files yet: the song's audio clips and the samples its devices play show here.",
                        juce::dontSendNotification);
    emptyPlugins.setText ("No plug-ins: those the song's tracks hold show here, with any that crashed or aren't installed.",
                          juce::dontSendNotification);
    unusedText.setJustificationType (juce::Justification::centredLeft);
    unusedText.setTitle ("Unused audio files");

    cleanUpButton.setButtonText ("Clean Up...");
    cleanUpButton.onClick = [this] { commands.invoke ("project.cleanUpAudio"); };
    addChildComponent (cleanUpButton);
    addAndMakeVisible (title);

    refreshTooltips();
    context.addListener (this);
    read();
}

ProjectView::~ProjectView()
{
    context.removeListener (this);
    endTyping();
    audioHover = nullptr;
    pluginHover = nullptr;
}

juce::TextEditor& ProjectView::getNotesField() noexcept
{
    return *notesField;
}

void ProjectView::refreshTooltips()
{
    notesField->setTooltip ("Notes about the song, kept in it: what it is, ideas, lyrics, what is left to do");
    audioList.setTooltip ("The audio files the song plays, with the tracks that play them; a missing one says so");
    pluginList.setTooltip ("The plug-ins the song holds, on their tracks, and any that crashed, aren't installed or are off");
    cleanUpButton.setTooltip (withShortcut (commands,
                                            "Clean Up Unused Audio: lists the audio files in the project's folder that nothing "
                                            "plays, any version included, to move those you tick to the recycle bin",
                                            "project.cleanUpAudio"));
}

void ProjectView::refreshNow()
{
    read();
}

//==============================================================================
void ProjectView::read()
{
    needsReading = false;
    ticksSinceRead = 0;

    // What shows changes only when what the model holds did: at rest, nothing repaints.
    auto newFiles = getProjectAudioFiles (context);
    auto newPlugins = getProjectPlugins (context);
    const auto newUnused = canCleanUpAudio (context) ? (int) findUnusedAudio (context).size() : 0;
    const auto changed = newFiles != audioFiles || newPlugins != plugins || newUnused != unusedFiles;
    readNotes();

    if (! changed)
        return;

    audioFiles = std::move (newFiles);
    plugins = std::move (newPlugins);
    unusedFiles = newUnused;

    const auto missing = (int) std::count_if (audioFiles.begin(), audioFiles.end(), [] (const auto& file) { return file.missing; });
    audioSection.set ("Audio Files", missing > 0 ? juce::String (missing) + " missing" : juce::String(), (int) audioFiles.size());
    pluginSection.set ("Plug-ins", describeProblems (plugins), (int) plugins.size());
    unusedText.setText (unusedFiles == 1 ? juce::String ("1 file in the project's folder is unused")
                                         : juce::String (unusedFiles) + " files in the project's folder are unused",
                        juce::dontSendNotification);

    audioList.updateContent();
    pluginList.updateContent();
    audioList.repaint();
    pluginList.repaint();
    resized();
}

void ProjectView::readNotes()
{
    // Not while a burst of typing goes on: the field holds what the song is getting.
    if (notesTyping != nullptr)
        return;

    if (const auto notes = getProjectNotes (context); notesField->getText() != notes)
        notesField->setText (notes, false);
}

void ProjectView::notesChanged()
{
    // A burst of typing is one undo step, from its first key to a pause, or until the keys go elsewhere.
    if (! context.hasProject())
        return;

    if (notesTyping == nullptr)
    {
        notesTyping = std::make_unique<SongGesture> (context, "Change Notes");

        if (! std::exchange (hearsPresses, true))
            juce::Desktop::getInstance().addGlobalMouseListener (pressElsewhere.get());
    }

    lastTyped = juce::Time::getMillisecondCounter();
    setProjectNotes (context, notesField->getText());
}

void ProjectView::endTyping()
{
    notesTyping = nullptr; // which keeps the burst's change as its step

    if (std::exchange (hearsPresses, false))
        juce::Desktop::getInstance().removeGlobalMouseListener (pressElsewhere.get());
}

void ProjectView::locate (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) audioFiles.size()) || ! audioFiles[(size_t) row].missing)
        return;

    const auto& missing = audioFiles[(size_t) row];
    const auto put = [safeThis = juce::Component::SafePointer<ProjectView> (this), path = missing.path] (const juce::File& found)
    {
        if (safeThis != nullptr && found.existsAsFile())
        {
            locateAudioFile (safeThis->context, path, found);
            safeThis->read();
        }
    };

    if (chooseReplacement != nullptr)
    {
        chooseReplacement (missing, put);
        return;
    }

    chooser =
        std::make_unique<juce::FileChooser> ("Locate " + missing.file.getFileName(), missing.file.getParentDirectory(), audioFilePattern);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [put] (const juce::FileChooser& chosen)
                          {
                              if (chosen.getResult() != juce::File())
                                  put (chosen.getResult());
                          });
}

void ProjectView::reload (int row)
{
    if (juce::isPositiveAndBelow (row, (int) plugins.size()) && plugins[(size_t) row].state == PluginState::crashed)
    {
        ui::reloadDevice (context, plugins[(size_t) row].device, pluginList);
        read();
    }
}

void ProjectView::showPlugin (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) plugins.size()))
        return;

    context.getSongSelection().selectOnly (plugins[(size_t) row].device);
    commands.invoke ("device.show");
}

//==============================================================================
juce::String ProjectView::describeAudioFile (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) audioFiles.size()))
        return {};

    const auto& file = audioFiles[(size_t) row];
    return file.file.getFileName() + (file.missing ? ", missing" : juce::String()) + ": " + file.channels.joinIntoString (", ");
}

juce::String ProjectView::describePlugin (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) plugins.size()))
        return {};

    const auto& plugin = plugins[(size_t) row];
    const auto state = describeState (plugin.state);
    return plugin.name + (state.isNotEmpty() ? ", " + state : juce::String()) + ": " + plugin.channelName;
}

bool ProjectView::showsAction (const juce::ListBox& list, const ListRowHover& hover, int row) const
{
    return row == hover.row || list.isRowSelected (row);
}

juce::Rectangle<int> ProjectView::getActionPosition (const juce::ListBox& list, int row) const
{
    // At the row's end, inside its inset (AudioModel and PluginModel's paintListBoxItem()).
    auto bounds = list.getRowPosition (row, true);
    return takeActionColumn (bounds);
}

juce::Rectangle<int> ProjectView::getLocatePosition (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) audioFiles.size()) || ! audioFiles[(size_t) row].missing
        || ! showsAction (audioList, *audioHover, row))
        return {};

    return getActionPosition (audioList, row);
}

juce::Rectangle<int> ProjectView::getReloadPosition (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) plugins.size()) || plugins[(size_t) row].state != PluginState::crashed
        || ! showsAction (pluginList, *pluginHover, row))
        return {};

    return getActionPosition (pluginList, row);
}

//==============================================================================
void ProjectView::projectChanged()
{
    endTyping(); // the closing project's song keeps what was typed into it
    notesField->setText ({}, false);
    audioFiles.clear();
    plugins.clear();
    unusedFiles = -1; // read anew
    read();
}

void ProjectView::songChanged (const SongChanges& changes)
{
    if (changes.notes)
        readNotes();

    if (changes.clips || changes.devices || changes.tracks)
        needsReading = true;
}

void ProjectView::visibilityChanged()
{
    // Read now as it shows, and looked at again while it does; nothing while it doesn't.
    if (isVisible())
    {
        read();
        startTimerHz (readsPerSecond);
    }
    else
    {
        endTyping();
        stopTimer();
    }
}

void ProjectView::timerCallback()
{
    // A burst of typing ends at a pause.
    if (notesTyping != nullptr && juce::Time::getMillisecondCounter() - lastTyped > typingPause)
        endTyping();

    if (isShowing() && (needsReading || ++ticksSinceRead >= ticksBetweenReads))
        read();
}

//==============================================================================
void ProjectView::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void ProjectView::resized()
{
    // As VS Code's side bar lays out a view of panes (SideBar): its title, then each pane's header and, while it is open,
    // what it holds, the lists as wide as the panel, the fields from its sides.
    auto area = getLocalBounds();
    title.setBounds (area.removeFromTop (SideBar::titleHeight));
    const auto fromSides = [] (juce::Rectangle<int> row)
    { return row.withTrimmedLeft (SideBar::fieldLeft).withTrimmedRight (SideBar::fieldRight); };

    // The notes, as tall as their lines as they wrap, between a few and a dozen.
    notesSection.setBounds (area.removeFromTop (notesSection.getPreferredHeight()));
    notesField->setVisible (notesSection.getToggleState());

    if (notesSection.getToggleState())
    {
        const auto lineHeight = juce::roundToInt (notesField->getFont().getHeight() * 1.2f);
        const auto padding = SideBar::controlHeight - lineHeight;
        const auto width = area.getWidth() - SideBar::fieldLeft - SideBar::fieldRight;
        juce::AttributedString text (notesField->getText().isEmpty() ? juce::String ("M") : notesField->getText());
        text.setFont (notesField->getFont());
        juce::TextLayout layout;
        layout.createLayout (text, (float) juce::jmax (1, width - padding));
        const auto lines = juce::jlimit (fewestNoteLines, mostNoteLines, layout.getNumLines());
        const auto height = padding + lineHeight * lines;
        notesField->setBounds (fromSides (area.removeFromTop (height + SideBar::inputRowHeight - SideBar::controlHeight))
                                   .withSizeKeepingCentre (width, height));
    }

    // The two lists share what is left, each as tall as its rows if there is room, with the clean-up row and the notes of
    // empty lists under their headers.
    const auto audioOpen = audioSection.getToggleState(), pluginsOpen = pluginSection.getToggleState();
    const auto showsUnused = audioOpen && unusedFiles > 0;
    const auto emptyHeight = 2 * ListRow::height;
    auto fixed = audioSection.getPreferredHeight() + pluginSection.getPreferredHeight() + (showsUnused ? SideBar::buttonRowHeight : 0)
                 + (audioOpen && audioFiles.empty() ? emptyHeight : 0) + (pluginsOpen && plugins.empty() ? emptyHeight : 0);
    const auto room = juce::jmax (0, area.getHeight() - fixed);
    auto audioHeight = audioOpen ? (int) audioFiles.size() * ListRow::height : 0;
    auto pluginHeight = pluginsOpen ? (int) plugins.size() * ListRow::height : 0;

    if (audioHeight + pluginHeight > room)
    {
        // Each gets its part of the room, at least a row while it has any.
        const auto total = (double) (audioHeight + pluginHeight);
        const auto audioShare = juce::roundToInt (room * (audioHeight / total));
        audioHeight = audioHeight > 0 ? juce::jmax (ListRow::height, audioShare) : 0;
        pluginHeight = pluginHeight > 0 ? juce::jmax (ListRow::height, room - audioHeight) : 0;
    }

    audioSection.setBounds (area.removeFromTop (audioSection.getPreferredHeight()));
    audioList.setBounds (area.removeFromTop (audioHeight));
    audioList.setVisible (audioOpen && ! audioFiles.empty());
    emptyAudio.setVisible (audioOpen && audioFiles.empty());
    unusedText.setVisible (showsUnused);
    cleanUpButton.setVisible (showsUnused);

    if (emptyAudio.isVisible())
        emptyAudio.setBounds (fromSides (area.removeFromTop (emptyHeight)).withTrimmedLeft (-emptyAudio.getBorderSize().getLeft()));

    if (showsUnused)
    {
        auto row = fromSides (area.removeFromTop (SideBar::buttonRowHeight));
        row = row.withSizeKeepingCentre (row.getWidth(), SideBar::controlHeight);
        const auto buttonWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (TextSize::body)),
                                                                                           cleanUpButton.getButtonText()))
                                 + 2 * Space::m;
        cleanUpButton.setBounds (row.removeFromRight (juce::jmin (row.getWidth() / 2, buttonWidth)));
        row.removeFromRight (Space::s);
        unusedText.setBounds (row.withTrimmedLeft (-unusedText.getBorderSize().getLeft()));
    }

    pluginSection.setBounds (area.removeFromTop (pluginSection.getPreferredHeight()));
    pluginList.setBounds (area.removeFromTop (pluginHeight));
    pluginList.setVisible (pluginsOpen && ! plugins.empty());
    emptyPlugins.setVisible (pluginsOpen && plugins.empty());

    if (emptyPlugins.isVisible())
        emptyPlugins.setBounds (fromSides (area.removeFromTop (emptyHeight)).withTrimmedLeft (-emptyPlugins.getBorderSize().getLeft()));
}
} // namespace phazite::ui
