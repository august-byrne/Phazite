// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppShell.h"
#include "CommandPalette.h"
#include "LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int topBarHeight = 44;
    constexpr int hintBarHeight = 24;
    constexpr int dividerSize = 5;

    // A StretchableLayoutManager item that is either shown with these sizes, or collapsed to nothing.
    void setItem (juce::StretchableLayoutManager& layout, int index, bool shown, double minimum, double maximum, double preferred)
    {
        layout.setItemLayout (index, shown ? minimum : 0.0, shown ? maximum : 0.0, shown ? preferred : 0.0);
    }
} // namespace

AppShell::AppShell (te::Engine& engine, CommandRegistry& registry, ComputerKeyboard& computerKeyboard, std::function<te::Edit*()> getEdit)
    : commands (registry),
      topBar (engine, registry, computerKeyboard, std::move (getEdit))
{
    setTitle ("Phazite");

    for (auto* c :
         std::initializer_list<juce::Component*> { &topBar, &browser, &browserDivider, &centre, &inspectorDivider, &inspector, &hintBar })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &playlist, &editorDivider, &editorDock })
        centre.addAndMakeVisible (c);

    centre.onResize = [this]
    {
        juce::Component* items[] = { &playlist, &editorDivider, &editorDock };
        rows.layOutComponents (items, 3, 0, 0, centre.getWidth(), centre.getHeight(), true, true);
    };

    setLayout (Layout::full);
}

void AppShell::setLayout (Layout layout)
{
    browserShown = layout != Layout::mix;
    inspectorShown = layout != Layout::beat;
    editorShown = true;
    editorShare = layout == Layout::mix ? 0.65 : 0.4;
    editorDock.showTab (layout == Layout::mix ? EditorDock::mixer : EditorDock::pianoRoll);
    updateLayout();
}

void AppShell::showEditor (EditorDock::Tab tab)
{
    // Like FL Studio's F7 and F9: show that editor, or hide it if it is already showing.
    if (editorShown && editorDock.getCurrentTabIndex() == tab)
    {
        editorShown = false;
    }
    else
    {
        editorShown = true;
        editorDock.showTab (tab);
    }

    updateLayout();
}

void AppShell::showCommandPalette()
{
    CommandPalette::show (commands, topBar.getSearchButton());
}

void AppShell::updateLayout()
{
    columns.clearAllItems();
    setItem (columns, 0, browserShown, 160, 480, 240);
    setItem (columns, 1, browserShown, dividerSize, dividerSize, dividerSize);
    columns.setItemLayout (2, 300, -1.0, -1.0);
    setItem (columns, 3, inspectorShown, dividerSize, dividerSize, dividerSize);
    setItem (columns, 4, inspectorShown, 180, 420, 260);

    rows.clearAllItems();
    rows.setItemLayout (0, 120, -1.0, editorShown ? -(1.0 - editorShare) : -1.0);
    setItem (rows, 1, editorShown, dividerSize, dividerSize, dividerSize);
    setItem (rows, 2, editorShown, 120, -0.85, -editorShare);

    browser.setVisible (browserShown);
    browserDivider.setVisible (browserShown);
    inspector.setVisible (inspectorShown);
    inspectorDivider.setVisible (inspectorShown);
    editorDock.setVisible (editorShown);
    editorDivider.setVisible (editorShown);

    resized();
    centre.resized();
}

void AppShell::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).border); // shows through the dividers
}

void AppShell::resized()
{
    auto area = getLocalBounds();
    topBar.setBounds (area.removeFromTop (topBarHeight));
    hintBar.setBounds (area.removeFromBottom (hintBarHeight));

    juce::Component* items[] = { &browser, &browserDivider, &centre, &inspectorDivider, &inspector };
    columns.layOutComponents (items, 5, area.getX(), area.getY(), area.getWidth(), area.getHeight(), false, true);
}

void AppShell::addCommands (CommandRegistry& registry)
{
    const auto add =
        [&registry] (const char* id, const char* name, const char* description, juce::StringArray synonyms, std::function<void()> perform)
    {
        registry.add ({ .id = id,
                        .name = name,
                        .category = "View",
                        .description = description,
                        .synonyms = std::move (synonyms),
                        .perform = std::move (perform) });
    };

    add ("view.commandPalette", "Search Commands", "Finds and runs any command by name.", { "palette", "find", "run" },
         [this] { showCommandPalette(); });

    add ("view.toggleBrowser", "Show or Hide the Browser", "Shows or hides the browser on the left.", { "files", "samples", "plugins" },
         [this]
         {
             browserShown = ! browserShown;
             updateLayout();
         });

    add ("view.toggleInspector", "Show or Hide the Inspector", "Shows or hides the inspector on the right.", { "properties" },
         [this]
         {
             inspectorShown = ! inspectorShown;
             updateLayout();
         });

    add ("view.toggleEditor", "Show or Hide the Editor", "Shows or hides the editor dock under the playlist.", { "dock" },
         [this]
         {
             editorShown = ! editorShown;
             updateLayout();
         });

    add ("view.pianoRoll", "Piano Roll", "Shows the piano roll, or hides it if it is showing.", { "notes", "midi" },
         [this] { showEditor (EditorDock::pianoRoll); });

    add ("view.mixer", "Mixer", "Shows the mixer, or hides it if it is showing.", { "channels", "faders" },
         [this] { showEditor (EditorDock::mixer); });

    add ("view.layoutBeat", "Beat Layout", "Arranges the panels for making beats: playlist, piano roll and browser.", { "layout" },
         [this] { setLayout (Layout::beat); });

    add ("view.layoutMix", "Mix Layout", "Arranges the panels for mixing: a large mixer and the inspector.", { "layout" },
         [this] { setLayout (Layout::mix); });

    add ("view.layoutFull", "Full Layout", "Shows every panel.", { "layout", "all" }, [this] { setLayout (Layout::full); });

    refreshShortcutTexts();
}

void AppShell::refreshShortcutTexts()
{
    topBar.refreshTooltips();

    const auto paletteKeys = commands.getShortcutDescriptions ("view.commandPalette");
    hintBar.setDefaultHint (paletteKeys.isEmpty() ? juce::String ("Search for any command with \"Search commands\" at the top right.")
                                                  : "Press " + paletteKeys[0] + " to search for any command.");
}
} // namespace phazite::ui
