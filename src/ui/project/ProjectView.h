// SPDX-License-Identifier: AGPL-3.0-or-later
// What the project holds, in the left panel (docs/design.md, section 4: the Project view), laid out as VS Code's side bar lays
// out a view of several panes (SideBar), each folding under its header:
// - Notes: the song's notes, typed in place, each burst of typing one undo step, "Change Notes";
// - Audio Files: each audio file the song plays, with the tracks that play it; a missing one marked so, with its Locate...
//   under the pointer, which takes a file in its place ("Locate Audio File"); and, while the project's folder has audio files
//   that nothing plays, how many, with Clean Up..., which runs Clean Up Unused Audio;
// - Plug-ins: each plug-in the song holds, on its track (or the master), and what is wrong with it, if anything: crashed
//   (with its Reload under the pointer), not installed here, or off; a click selects it and shows it in the Devices view.
// By the model's functions (model/project/ProjectContents.h) and the commands. It reads them again once a turn after the song
// changes, and every few seconds while it shows, for files that came or went on the disk.
#pragma once

#include "model/commands/SongGesture.h"
#include "model/project/ProjectContents.h"
#include "model/project/ProjectContext.h"
#include "ui/components/ListRowHover.h"
#include "ui/components/SectionHeader.h"
#include "ui/components/ViewTitle.h"
#include "ui/input/CommandBridge.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace phazite::ui
{
class ProjectView : public juce::Component,
                    private ProjectContext::Listener,
                    private juce::Timer
{
public:
    ProjectView (CommandBridge&, ProjectContext&);
    ~ProjectView() override;

    void refreshTooltips(); // with the commands' keys, after the keymap changed

    // What the model holds now, read at once (it is read again by itself after changes, and every few seconds while shown).
    void refreshNow();

    // Asks for the file to put in a missing one's place, and calls back with it (none if the question is cancelled): a file
    // chooser, unless this is set (the tests answer it).
    std::function<void (const ProjectAudioFile&, std::function<void (const juce::File&)>)> chooseReplacement;

    // For tests: what the lists show, a row at a time, as screen readers say it, and the parts. A missing file's Locate... and
    // a crashed plug-in's Reload show on their row under the pointer, or selected; empty while they don't show.
    juce::TextEditor& getNotesField() noexcept;
    juce::Button& getNotesSection() noexcept { return notesSection; }
    juce::Button& getAudioSection() noexcept { return audioSection; }
    juce::Button& getPluginSection() noexcept { return pluginSection; }
    juce::ListBox& getAudioList() noexcept { return audioList; }
    juce::ListBox& getPluginList() noexcept { return pluginList; }
    int getNumAudioFiles() const noexcept { return (int) audioFiles.size(); }
    int getNumPlugins() const noexcept { return (int) plugins.size(); }
    juce::String describeAudioFile (int row) const;
    juce::String describePlugin (int row) const;
    juce::Rectangle<int> getLocatePosition (int row) const;
    juce::Rectangle<int> getReloadPosition (int row) const;
    juce::Button& getCleanUpButton() noexcept { return cleanUpButton; }
    juce::String getUnusedText() const { return unusedText.getText(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    class AudioModel;
    class PluginModel;
    class NotesField;

    CommandBridge& commands;
    ProjectContext& context;

    ViewTitle title { "Project" };
    SectionHeader notesSection { "Notes", SectionHeader::Style::pane }, audioSection { "Audio Files", SectionHeader::Style::pane },
        pluginSection { "Plug-ins", SectionHeader::Style::pane };
    std::unique_ptr<NotesField> notesField;
    std::unique_ptr<SongGesture> notesTyping; // a burst of typing's one undo step, until a pause or the keys go elsewhere
    juce::uint32 lastTyped = 0;               // when the last key of the burst changed the notes
    std::unique_ptr<AudioModel> audioModel;
    std::unique_ptr<PluginModel> pluginModel;
    juce::ListBox audioList, pluginList;
    std::unique_ptr<ListRowHover> audioHover, pluginHover;
    juce::Label emptyAudio, emptyPlugins, unusedText;
    juce::TextButton cleanUpButton;
    std::unique_ptr<juce::FileChooser> chooser;

    std::vector<ProjectAudioFile> audioFiles;
    std::vector<ProjectPlugin> plugins;
    int unusedFiles = -1;     // none read yet
    bool needsReading = true; // the song changed since the lists were read
    int ticksSinceRead = 0;

    // While a burst of typing goes on: a press anywhere but in the notes ends it, so that what it starts is a step of its own.
    struct PressElsewhere;
    std::unique_ptr<PressElsewhere> pressElsewhere;
    bool hearsPresses = false;

    // The rows' icons and actions, drawn in each row's colours.
    TintedIcon soundIcon { Icon::sound }, instrumentIcon { Icon::instrument }, effectIcon { Icon::effect };
    TintedIcon locateIcon { Icon::search }, reloadIcon { Icon::reload };

    void read();         // the lists, the notes and the unused files, from the model
    void readNotes();    // the notes, unless a burst of typing is going on
    void notesChanged(); // typed: into the song, joining the burst's step
    void endTyping();    // the burst's step kept
    void locate (int row);
    void reload (int row);
    void showPlugin (int row);
    bool showsAction (const juce::ListBox&, const ListRowHover&, int row) const; // under the pointer, or selected
    juce::Rectangle<int> getActionPosition (const juce::ListBox&, int row) const;

    void projectChanged() override;
    void songChanged (const SongChanges&) override;
    void versionsChanged() override { needsReading = true; } // what the versions play is in use too (UnusedAudio.h)
    void timerCallback() override;
};
} // namespace phazite::ui
