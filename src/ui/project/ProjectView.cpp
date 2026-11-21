// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectView.h"
#include "devices/core/Clock.h"
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
    constexpr int ticksBetweenReads = 5 * readsPerSecond; // the plug-ins' states change by themselves: read again so often
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
            case PluginState::loading:
                return "loading";
            case PluginState::playing:
            default:
                return {};
        }
    }

    // What a header says is wrong, or still to come, after its name: "2 missing"; "1 crashed, 1 not installed"; "3 loading".
    juce::String describeProblems (const std::vector<ProjectPlugin>& plugins)
    {
        juce::StringArray problems;

        for (const auto state : { PluginState::crashed, PluginState::missing, PluginState::off, PluginState::loading })
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
            case PluginState::loading:
                return who + ": loading. It plays once it has loaded, and is silent meanwhile";
            case PluginState::playing:
            default:
                return who + ". Click to show it";
        }
    }

private:
    ProjectView& view;
};

//==============================================================================
// The MIDI links: each with the MIDI icon, its controller and what it turns; its Remove under the pointer or selected. A click
// selects what it turns and shows it.
class ProjectView::LinkModel : public juce::ListBoxModel
{
public:
    explicit LinkModel (ProjectView& v)
        : view (v)
    {
    }

    int getNumRows() override { return view.getNumLinks(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;

        const auto& p = getPalette (view);
        const auto& link = view.links[(size_t) row];
        const auto textColour = drawListRow (g, view, juce::Rectangle<int> (width, height).toFloat(), selected, row == view.linkHover->row);
        auto area = juce::Rectangle<int> (width, height);
        view.midiIcon.draw (g, takeIcon (area).toFloat().withSizeKeepingCentre (History::rowIconSize, History::rowIconSize), p.textMuted);
        const auto action = takeActionColumn (area);

        if (view.showsAction (view.linkList, *view.linkHover, row))
            view.removeIcon.draw (g, action.toFloat().withSizeKeepingCentre (History::rowIconSize, History::rowIconSize), p.text);

        drawText (g, area, textColour, p.textMuted, describeMidiController (link.controller), link.turns);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        if (view.getRemovePosition (row).contains (e.getEventRelativeTo (&view.linkList).getPosition()))
            view.removeLink (row);
        else
            view.showLink (row);
    }

    void returnKeyPressed (int row) override { view.showLink (row); }
    void selectedRowsChanged (int) override { view.linkList.repaint(); }

    juce::String getNameForRow (int row) override { return view.describeLink (row); }

    juce::String getTooltipForRow (int row) override
    {
        return juce::isPositiveAndBelow (row, getNumRows())
                   ? view.describeLink (row) + ". Click to show it; its cross removes the link (Remove MIDI Link)"
                   : juce::String();
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
      linkModel (std::make_unique<LinkModel> (*this)),
      audioList ("Audio Files", audioModel.get()),
      pluginList ("Plug-ins", pluginModel.get()),
      linkList ("MIDI Links", linkModel.get()),
      unusedFinder (projectContext),
      pressElsewhere (std::make_unique<PressElsewhere> (*this))
{
    setTitle ("Project");
    setWantsKeyboardFocus (true); // its icon gives it the keys (and a screen reader its name), and Tab goes on into its parts
    const auto& p = getPalette (*this);

    // The first pane has no line above it, as in VS Code; each folds under its header.
    notesSection.setLineAbove (false);

    for (auto* section : { &notesSection, &audioSection, &pluginSection, &linkSection })
    {
        section->onClick = [this] { resized(); };
        addAndMakeVisible (section);
    }

    notesSection.set ("Notes", {}, 0);
    audioSection.set ("Audio Files", {}, 0);
    pluginSection.set ("Plug-ins", {}, 0);
    linkSection.set ("MIDI Links", {}, 0);
    notesField->setTitle ("Notes");
    notesField->setMultiLine (true, true);
    notesField->setReturnKeyStartsNewLine (true);
    notesField->setScrollbarsShown (true);
    notesField->setFont (juce::FontOptions (TextSize::body));
    notesField->setTextToShowWhenEmpty ("Notes about the song: ideas, lyrics, what is left to do", p.textMuted);
    notesField->onTextChange = [this] { notesChanged(); };
    addAndMakeVisible (*notesField);

    for (auto* list : { &audioList, &pluginList, &linkList })
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
    linkHover = std::make_unique<ListRowHover> (linkList, repaintRows (linkList));

    for (auto* label : { &emptyAudio, &emptyPlugins, &emptyLinks, &unusedText })
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
    emptyLinks.setText ("No MIDI links. Right-click a control and choose MIDI Learn.", juce::dontSendNotification);
    unusedText.setJustificationType (juce::Justification::centredLeft);
    unusedText.setTitle ("Unused audio files");

    cleanUpButton.setButtonText ("Clean Up...");
    cleanUpButton.onClick = [this] { commands.invoke ("project.cleanUpAudio"); };
    addChildComponent (cleanUpButton);
    addAndMakeVisible (title);

    refreshTooltips();
    unusedFinder.onFound = [this] { showUnused(); };
    missingCheck.onChecked = [this] { read (false); };
    context.addListener (this);
    read (true);
}

ProjectView::~ProjectView()
{
    context.removeListener (this);
    endTyping();
    audioHover = nullptr;
    pluginHover = nullptr;
    linkHover = nullptr;
}

juce::TextEditor& ProjectView::getNotesField() noexcept
{
    return *notesField;
}

void ProjectView::refreshTooltips()
{
    notesSection.setTooltip ("Folds the notes written about the song, or shows them again");
    audioSection.setTooltip ("Folds the song's audio files, or shows them again");
    pluginSection.setTooltip ("Folds the song's plug-ins, or shows them again");
    linkSection.setTooltip ("Folds the song's MIDI links, or shows them again");
    notesField->setTooltip ("Notes about the song, kept in it: what it is, ideas, lyrics, what is left to do");
    audioList.setTooltip ("The audio files the song plays, with the tracks that play them; a missing one says so");
    pluginList.setTooltip ("The plug-ins the song holds, on their tracks, and any that crashed, aren't installed or are off");
    linkList.setTooltip ("The knobs and faders of MIDI controllers linked to the song's parameters, and what each turns");
    cleanUpButton.setTooltip (withShortcut (commands,
                                            "Clean Up Unused Audio: lists the audio files in the project's folder that nothing "
                                            "plays, any version included, to move those you tick to the recycle bin",
                                            "project.cleanUpAudio"));
}

void ProjectView::refreshNow()
{
    read (true);
}

//==============================================================================
void ProjectView::read (bool look)
{
    needsReading = false;
    ticksSinceRead = 0;

    // What shows changes only when what the model holds did: at rest, nothing repaints. No disk: which files are missing is
    // what the last check found.
    auto newFiles = getProjectAudioFiles (context, missingCheck.getMissing());
    auto newPlugins = getProjectPlugins (context);
    auto newLinks = getMidiLinks (context);
    const auto changed = newFiles != audioFiles || newPlugins != plugins || newLinks != links;
    readNotes();

    // While it shows, and only once something that could change the answers happened: which files are missing, and which in
    // the project's folder are unused, looked for in the background, what was known shown until the answers come
    // (onChecked, showUnused()).
    if (look && isShowing())
    {
        juce::Array<juce::File> files;

        for (const auto& file : newFiles)
            files.add (file.file);

        missingCheck.check (files);
        unusedFinder.find();
    }

    showUnused(); // the button greyed while it can't look

    if (! changed)
        return;

    audioFiles = std::move (newFiles);
    plugins = std::move (newPlugins);
    links = std::move (newLinks);

    const auto missing = (int) std::count_if (audioFiles.begin(), audioFiles.end(), [] (const auto& file) { return file.missing; });
    audioSection.set ("Audio Files", missing > 0 ? juce::String (missing) + " missing" : juce::String(), (int) audioFiles.size());
    pluginSection.set ("Plug-ins", describeProblems (plugins), (int) plugins.size());
    linkSection.set ("MIDI Links", {}, (int) links.size());

    audioList.updateContent();
    pluginList.updateContent();
    linkList.updateContent();
    audioList.repaint();
    linkList.repaint();
    pluginList.repaint();
    resized();
}

void ProjectView::showUnused()
{
    // The last look's count, the button there while a version or a snapshot is written (it can't look then), greyed with
    // why; or why it can't tell which files are unused.
    const auto& found = unusedFinder.getFound();
    const auto problem = found.has_value() && ! found->unreadable.isEmpty() ? found->unreadable[0] : juce::String();
    const auto newUnused = found.has_value() ? (int) found->files.size() : 0;
    const auto why =
        problem.isNotEmpty() ? "Phazite can't tell which audio files are unused: " + problem + "." : getWhyNotCleanUpAudio (context);
    cleanUpButton.setEnabled (why.isEmpty());
    cleanUpButton.setTooltip (why.isNotEmpty() ? why
                                               : withShortcut (commands,
                                                               "Clean Up Unused Audio: lists the audio files in the project's folder "
                                                               "that nothing plays, any version included, to move those you tick to "
                                                               "the recycle bin",
                                                               "project.cleanUpAudio"));

    const auto sameCount = std::exchange (unusedFiles, newUnused) == newUnused;
    const auto sameProblem = std::exchange (unusedProblem, problem) == problem;

    if (sameCount && sameProblem)
        return;

    unusedText.setText (unusedProblem.isNotEmpty() ? juce::String ("Can't tell which audio files are unused yet")
                        : unusedFiles == 1         ? juce::String ("1 file in the project's folder is unused")
                                                   : juce::String (unusedFiles) + " files in the project's folder are unused",
                        juce::dontSendNotification);
    resized();
}

void ProjectView::readNotes()
{
    // Not while a burst of typing goes on: the field holds what the song is getting. Nor while the field holds what was
    // typed and it hasn't told of yet (a text field tells of a change a message later): a read between the two, which the
    // check of the missing files' answer can be, put the song's notes back over what was typed, and the change the field
    // then told of was those.
    if (notesTyping != nullptr || notesField->getText() != notesSynced)
        return;

    if (const auto notes = getProjectNotes (context); notesSynced != notes)
    {
        notesSynced = notes;
        notesField->setText (notes, false);
    }
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

    lastTyped = devices::Clock::getMillisecondCounter();
    notesSynced = notesField->getText();
    setProjectNotes (context, notesSynced);
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
            safeThis->read (true);
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
        read (false);
    }
}

void ProjectView::showPlugin (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) plugins.size()))
        return;

    context.getSongSelection().selectOnly (plugins[(size_t) row].device);
    commands.invoke ("device.show");
}

void ProjectView::removeLink (int row)
{
    if (juce::isPositiveAndBelow (row, (int) links.size()))
    {
        // Read again after the click, not inside it: the list's row that was clicked goes with the link.
        removeMidiLink (context, links[(size_t) row].controller);
        needsReading = true;
    }
}

void ProjectView::showLink (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) links.size()))
        return;

    // Its device in the Devices view, as a plug-in's row shows it; or else its track (a fader's, a send's, a macro's).
    if (const auto& link = links[(size_t) row]; link.device.isValid())
    {
        context.getSongSelection().selectOnly (link.device);
        commands.invoke ("device.show");
    }
    else if (link.track.isValid())
    {
        context.getSongSelection().selectOnly (link.track);
    }
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

juce::String ProjectView::describeLink (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) links.size()))
        return {};

    const auto& link = links[(size_t) row];
    return describeMidiController (link.controller) + ": " + link.turns;
}

juce::Rectangle<int> ProjectView::getRemovePosition (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) links.size()) || ! showsAction (linkList, *linkHover, row))
        return {};

    return getActionPosition (linkList, row);
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
    notesSynced.clear();
    audioFiles.clear();
    plugins.clear();
    links.clear();
    unusedFiles = 0; // none shown until the new project's are found
    unusedProblem.clear();
    read (true);
    resized(); // the count of the project before hidden, even where the lists are the same (read() lays out what changed)
}

void ProjectView::songChanged (const SongChanges& changes)
{
    if (changes.notes)
        readNotes();

    // What a link turns is named by its track, its device and its parameter (a macro's or a modulator's by theirs).
    if (changes.clips || changes.devices || changes.tracks || changes.modulation || changes.midiLinks)
        needsReading = true;
}

void ProjectView::visibilityChanged()
{
    // Read now as it shows, the disk looked at again, and the model read again while it does; nothing while it doesn't.
    if (isVisible())
    {
        wasInFront = askInFront();
        deletionsSeen = UnusedAudioFinder::countDeletions();
        read (true);
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
    if (notesTyping != nullptr && devices::Clock::getMillisecondCounter() - lastTyped > typingPause)
        endTyping();

    // The disk looked at again once something could have changed what it says: the song or its versions changed (a file
    // played or no more: songChanged(), versionsChanged()), Phazite came to the front again (the user back from Explorer or
    // another program, where files may have gone or come), or Clean Up deleted files. The model alone read again every few
    // seconds meanwhile (the plug-ins' states).
    const auto inFront = askInFront();
    const auto cameForward = inFront && ! wasInFront;
    wasInFront = inFront;
    const auto deletions = UnusedAudioFinder::countDeletions();
    const auto deleted = deletions != deletionsSeen;
    deletionsSeen = deletions;

    // And a look Clean Up refused (the versions being read or written, an import under way) once it can look.
    if (isShowing() && (needsReading || cameForward || deleted || unusedFinder.shouldLookAgain()))
        read (true);
    else if (isShowing() && ++ticksSinceRead >= ticksBetweenReads)
        read (false);
}

//==============================================================================
bool ProjectView::askInFront() const
{
    return isInFront != nullptr ? isInFront() : juce::Process::isForegroundProcess();
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

    // The three lists share what is left, each as tall as its rows if there is room, with the clean-up row and the notes of
    // empty lists under their headers.
    const auto audioOpen = audioSection.getToggleState(), pluginsOpen = pluginSection.getToggleState(),
               linksOpen = linkSection.getToggleState();
    const auto showsUnused = audioOpen && (unusedFiles > 0 || unusedProblem.isNotEmpty());
    const auto emptyHeight = 2 * ListRow::height;
    auto fixed = audioSection.getPreferredHeight() + pluginSection.getPreferredHeight() + linkSection.getPreferredHeight()
                 + (showsUnused ? SideBar::buttonRowHeight : 0) + (audioOpen && audioFiles.empty() ? emptyHeight : 0)
                 + (pluginsOpen && plugins.empty() ? emptyHeight : 0) + (linksOpen && links.empty() ? emptyHeight : 0);
    const auto room = juce::jmax (0, area.getHeight() - fixed);
    auto audioHeight = audioOpen ? (int) audioFiles.size() * ListRow::height : 0;
    auto pluginHeight = pluginsOpen ? (int) plugins.size() * ListRow::height : 0;
    auto linkHeight = linksOpen ? (int) links.size() * ListRow::height : 0;

    if (audioHeight + pluginHeight + linkHeight > room)
    {
        // Each gets its part of the room, at least a row while it has any.
        const auto total = (double) (audioHeight + pluginHeight + linkHeight);
        const auto share = [room, total] (int height)
        { return height > 0 ? juce::jmax (ListRow::height, juce::roundToInt (room * (height / total))) : 0; };
        const auto audioShare = share (audioHeight), pluginShare = share (pluginHeight);
        linkHeight = linkHeight > 0 ? juce::jmax (ListRow::height, room - audioShare - pluginShare) : 0;
        audioHeight = audioShare;
        pluginHeight = pluginShare;
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

    linkSection.setBounds (area.removeFromTop (linkSection.getPreferredHeight()));
    linkList.setBounds (area.removeFromTop (linkHeight));
    linkList.setVisible (linksOpen && ! links.empty());
    emptyLinks.setVisible (linksOpen && links.empty());

    if (emptyLinks.isVisible())
        emptyLinks.setBounds (fromSides (area.removeFromTop (emptyHeight)).withTrimmedLeft (-emptyLinks.getBorderSize().getLeft()));
}
} // namespace phazite::ui
