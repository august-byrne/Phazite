// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppShell.h"
#include "CommandPalette.h"
#include "model/mixer/MixerModel.h"
#include "model/presets/PresetModel.h"
#include "ui/browser/PluginPicker.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int topBarHeight = 44;
    constexpr int hintBarHeight = 24;
    constexpr int dividerSize = 5;
    constexpr int detachButtonSize = 26;

    // A StretchableLayoutManager item that is either shown with these sizes, or collapsed to nothing.
    void setItem (juce::StretchableLayoutManager& layout, int index, bool shown, double minimum, double maximum, double preferred)
    {
        layout.setItemLayout (index, shown ? minimum : 0.0, shown ? maximum : 0.0, shown ? preferred : 0.0);
    }

    // Where a panel's own window goes: on another monitor if there is one, otherwise over the main window.
    juce::Rectangle<int> choosePanelWindowBounds (const juce::Component& mainWindowContent, juce::Point<int> size)
    {
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto mainCentre = mainWindowContent.getScreenBounds().getCentre();
        const auto bounds = juce::Rectangle<int> (size.x, size.y);

        for (const auto& display : displays.displays)
            if (! display.userBounds.contains (mainCentre.toFloat()))
                return bounds.withCentre (display.userBounds.getCentre().roundToInt());

        if (const auto* display = displays.getDisplayForPoint (mainCentre.toFloat()))
            return bounds.withCentre (mainCentre).constrainedWithin (display->userBounds.getSmallestIntegerContainer());

        return bounds.withCentre (mainCentre);
    }
} // namespace

//==============================================================================
// A panel in a window of its own. The shortcuts work there too, and closing the window puts the panel
// back into the main window.
struct AppShell::PanelWindow : public juce::DocumentWindow
{
    PanelWindow (juce::Component& panel, juce::Rectangle<int> bounds, juce::KeyListener& keys, std::function<void()> onClose)
        : juce::DocumentWindow (panel.getTitle() + " - Phazite", getPalette (panel).background, juce::DocumentWindow::allButtons),
          shortcuts (keys),
          close (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        setContentNonOwned (&panel, false);
        setResizable (true, false);
        setResizeLimits (240, 240, 16384, 16384);
        setBounds (bounds);
        addKeyListener (&shortcuts);
        setVisible (true);
    }

    ~PanelWindow() override
    {
        removeKeyListener (&shortcuts);
        clearContentComponent(); // the panel belongs to the main window
    }

    void closeButtonPressed() override { close(); }

    juce::KeyListener& shortcuts;
    std::function<void()> close;
};

//==============================================================================
AppShell::AppShell (EngineContext& engine, CommandBridge& bridge, KeyDispatcher& keys, ComputerKeyboard& keyboard, ProjectContext& context,
                    Settings& s, SoundPack& pack, Library& l, Previewer& p)
    : commands (bridge),
      shortcutKeys (keys),
      projectContext (context),
      library (l),
      previewer (p),
      computerKeyboard (keyboard),
      topBar (engine, bridge, keyboard, context),
      browser (library, previewer, pack, s, context),
      playlist (bridge, context),
      inspector (bridge, context),
      pianoRoll (bridge, context),
      mixer (bridge, context),
      guide (bridge, s)
{
    setTitle ("Phazite");

    // The guide's steps that no command does.
    computerKeyboard.onNotePlayed = [this] { guide.markDone (Guide::Step::playNotes); };
    browser.onUsed = [this] (const LibraryItem&) { guide.markDone (Guide::Step::addSound); };
    hintBar.setDefaultHint ([this] { return getDefaultHint(); });

    // Double-clicking a MIDI clip opens it in the piano roll, which then has the keys.
    playlist.onOpenClip = [this] (ClipID clip)
    {
        pianoRoll.showClip (clip);
        revealEditor (EditorDock::pianoRoll);
        pianoRoll.grabKeyboardFocus();
        setActiveEditor (&pianoRoll);
    };

    // Enter or a double-click in the browser, and the plug-in picker.
    browser.onPlace = [this] (const LibraryItem& item) { return placeItem (item); };
    browser.onShowSoundPack = [this] { commands.invoke ("help.phaziteSounds"); };

    for (auto* c : std::initializer_list<juce::Component*> { &topBar, &browser, &browserDivider, &centre, &inspectorDivider, &inspector,
                                                             &hintBar, &browserDetachButton, &inspectorDetachButton })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &playlist, &editorDivider, &editorDock, &editorDetachButton })
        centre.addAndMakeVisible (c);

    centre.onResize = [this]
    {
        // A panel in its own window is that window's to lay out: an empty space here.
        juce::Component* items[] = { &playlist, &editorDivider, editorWindow == nullptr ? &editorDock : nullptr };
        rows.layOutComponents (items, 3, 0, 0, centre.getWidth(), centre.getHeight(), true, true);
        placeDetachButtons();
    };

    for (const auto which : { DetachablePanel::browser, DetachablePanel::inspector, DetachablePanel::editor })
    {
        auto& button = getDetachButton (which);
        button.setTooltip ("Moves this panel to a window of its own, for example on a second monitor. Close that window to put it back");
        button.onClick = [this, which] { setDetached (which, true); };
    }

    // Clicking in an editor makes it the one the time line's commands act on; so does the keyboard focus
    // moving into it.
    playlist.onUsed = [this] { setActiveEditor (&playlist); };
    pianoRoll.onUsed = [this] { setActiveEditor (&pianoRoll); };
    setActiveEditor (&playlist);
    juce::Desktop::getInstance().addFocusChangeListener (this);
    projectContext.addListener (this);
    setLayout (Layout::full);
}

AppShell::~AppShell() // here, where PanelWindow is complete
{
    projectContext.removeListener (this);
    computerKeyboard.onNotePlayed = nullptr; // the keyboard is the main window's, and outlives this
    juce::Desktop::getInstance().removeFocusChangeListener (this);
}

void AppShell::globalFocusChanged (juce::Component* focused)
{
    // Focus that moves elsewhere (to the mixer, say) leaves the last editor active.
    for (auto* c = focused; c != nullptr; c = c->getParentComponent())
    {
        if (auto* editor = dynamic_cast<TimelineEditor*> (c))
        {
            setActiveEditor (editor);
            return;
        }
    }
}

void AppShell::setActiveEditor (TimelineEditor* editor)
{
    activeEditor = editor;

    for (auto* each : std::initializer_list<TimelineEditor*> { &playlist, &pianoRoll })
        each->setActive (each == editor);
}

void AppShell::projectChanged()
{
    previewer.stop(); // which may be in time with the project that closes

    // A menu's choices name the items of the project that was open, and the new one's can have the same IDs.
    juce::PopupMenu::dismissAllActiveMenus();
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
    // In its own window, the editor just shows that tab.
    if (editorWindow != nullptr)
    {
        editorDock.showTab (tab);
        editorWindow->toFront (true);
        return;
    }

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

void AppShell::revealEditor (EditorDock::Tab tab)
{
    editorDock.showTab (tab);

    if (editorWindow != nullptr)
    {
        editorWindow->toFront (true);
        return;
    }

    editorShown = true;
    updateLayout();
}

void AppShell::showCommandPalette()
{
    CommandPalette::show (commands, topBar.getSearchButton());
}

void AppShell::showPluginPicker()
{
    PluginPicker::show (library, topBar.getSearchButton(),
                        [safeThis = juce::Component::SafePointer<AppShell> (this)] (const LibraryItem& item)
                        {
                            if (safeThis != nullptr)
                                safeThis->browser.place (item);
                        });
}

bool AppShell::placeItem (const LibraryItem& item)
{
    if (! projectContext.hasProject())
        return false;

    // A device goes onto the selected track (or a new one: phazite::addDeviceTo()), and so does a preset (a track's makes
    // a new track after it: loadPreset()); a sound goes at the play start.
    const auto tracks = projectContext.getSongSelection().getTracks();
    const auto track = tracks.empty() ? TrackID() : tracks.front();

    if (item.device.has_value())
        return addDeviceTo (projectContext, track, *item.device);

    if (item.kind == LibraryItem::Kind::preset)
        return loadPreset (projectContext, item.file, track);

    return playlist.placeFile (item.file);
}

//==============================================================================
void AppShell::setDetached (DetachablePanel which, bool detached)
{
    auto& window = getWindow (which);

    if (detached == (window != nullptr))
    {
        if (window != nullptr)
            window->toFront (true);

        return;
    }

    auto& panel = getPanel (which);

    if (detached)
    {
        const juce::Point<int> size (juce::jmax (panel.getWidth(), 360), juce::jmax (panel.getHeight(), 400));

        window = std::make_unique<PanelWindow> (panel, choosePanelWindowBounds (*this, size), shortcutKeys,
                                                [safeThis = juce::Component::SafePointer<AppShell> (this), which]
                                                {
                                                    // Closing deletes the window, so dock the panel once the click is over.
                                                    juce::MessageManager::callAsync (
                                                        [safeThis, which]
                                                        {
                                                            if (safeThis != nullptr)
                                                                safeThis->setDetached (which, false);
                                                        });
                                                });
        panel.setVisible (true);
    }
    else
    {
        window = nullptr; // gives the panel back
        auto& parent = which == DetachablePanel::editor ? static_cast<juce::Component&> (centre) : static_cast<juce::Component&> (*this);
        parent.addChildComponent (panel);
        getDetachButton (which).toFront (false);
    }

    updateLayout();
}

bool AppShell::isDetached (DetachablePanel which) const
{
    return getWindow (which) != nullptr;
}

juce::Component& AppShell::getPanel (DetachablePanel which)
{
    switch (which)
    {
        case DetachablePanel::browser:
            return browser;
        case DetachablePanel::inspector:
            return inspector;
        case DetachablePanel::editor:
        default:
            return editorDock;
    }
}

IconButton& AppShell::getDetachButton (DetachablePanel which)
{
    switch (which)
    {
        case DetachablePanel::browser:
            return browserDetachButton;
        case DetachablePanel::inspector:
            return inspectorDetachButton;
        case DetachablePanel::editor:
        default:
            return editorDetachButton;
    }
}

std::unique_ptr<AppShell::PanelWindow>& AppShell::getWindow (DetachablePanel which)
{
    switch (which)
    {
        case DetachablePanel::browser:
            return browserWindow;
        case DetachablePanel::inspector:
            return inspectorWindow;
        case DetachablePanel::editor:
        default:
            return editorWindow;
    }
}

const std::unique_ptr<AppShell::PanelWindow>& AppShell::getWindow (DetachablePanel which) const
{
    return const_cast<AppShell&> (*this).getWindow (which);
}

//==============================================================================
void AppShell::updateLayout()
{
    // A panel in its own window takes no room here.
    const bool browserDocked = browserShown && browserWindow == nullptr;
    const bool inspectorDocked = inspectorShown && inspectorWindow == nullptr;
    const bool editorDocked = editorShown && editorWindow == nullptr;

    columns.clearAllItems();
    setItem (columns, 0, browserDocked, 160, 480, 240);
    setItem (columns, 1, browserDocked, dividerSize, dividerSize, dividerSize);
    columns.setItemLayout (2, 300, -1.0, -1.0);
    setItem (columns, 3, inspectorDocked, dividerSize, dividerSize, dividerSize);
    setItem (columns, 4, inspectorDocked, 180, 420, 260);

    rows.clearAllItems();
    rows.setItemLayout (0, 120, -1.0, editorDocked ? -(1.0 - editorShare) : -1.0);
    setItem (rows, 1, editorDocked, dividerSize, dividerSize, dividerSize);
    setItem (rows, 2, editorDocked, 120, -0.85, -editorShare);

    // Panels in their own windows stay visible there.
    if (browserWindow == nullptr)
        browser.setVisible (browserDocked);

    if (inspectorWindow == nullptr)
        inspector.setVisible (inspectorDocked);

    if (editorWindow == nullptr)
        editorDock.setVisible (editorDocked);

    browserDivider.setVisible (browserDocked);
    inspectorDivider.setVisible (inspectorDocked);
    editorDivider.setVisible (editorDocked);
    browserDetachButton.setVisible (browserDocked);
    inspectorDetachButton.setVisible (inspectorDocked);
    editorDetachButton.setVisible (editorDocked);

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

    // A panel in its own window is that window's to lay out: an empty space here.
    juce::Component* items[] = { browserWindow == nullptr ? &browser : nullptr, &browserDivider, &centre, &inspectorDivider,
                                 inspectorWindow == nullptr ? &inspector : nullptr };
    columns.layOutComponents (items, 5, area.getX(), area.getY(), area.getWidth(), area.getHeight(), false, true);
    placeDetachButtons();
}

void AppShell::placeDetachButtons()
{
    const auto corner = [] (juce::Rectangle<int> panelBounds)
    {
        return panelBounds.removeFromTop (detachButtonSize + Space::xs)
            .removeFromRight (detachButtonSize + Space::xs)
            .reduced (Space::xs / 2);
    };

    if (browserWindow == nullptr)
        browserDetachButton.setBounds (corner (browser.getBounds()));

    if (inspectorWindow == nullptr)
        inspectorDetachButton.setBounds (corner (inspector.getBounds()));

    if (editorWindow == nullptr)
        editorDetachButton.setBounds (corner (editorDock.getBounds())); // both in centre's coordinates
}

//==============================================================================
void AppShell::addCommands (CommandRegistry& registry)
{
    const auto add = [&registry] (const char* id, const char* name, const char* description, juce::StringArray synonyms,
                                  std::function<void()> perform, std::function<bool()> isEnabled = {})
    {
        registry.add ({ .id = id,
                        .name = name,
                        .category = "View",
                        .description = description,
                        .synonyms = std::move (synonyms),
                        .perform = std::move (perform),
                        .isEnabled = std::move (isEnabled) });
    };

    add ("view.commandPalette", "Search Commands", "Finds and runs any command by name.", { "palette", "find", "run" },
         [this] { showCommandPalette(); });

    add ("view.searchBrowser", "Search the Browser", "Shows the browser and puts the cursor in its search field.",
         { "find", "sounds", "samples", "plugins" },
         [this]
         {
             if (browserWindow != nullptr)
             {
                 browserWindow->toFront (true);
             }
             else if (! browserShown)
             {
                 browserShown = true;
                 updateLayout();
             }

             browser.focusSearch();
         });

    registry.add ({ .id = "device.add",
                    .name = "Add Instrument or Effect",
                    .category = "Device",
                    .description = "Finds an instrument or effect by name and adds it to the selected track, as FL Studio's plug-in "
                                   "picker does.",
                    .synonyms = { "plug-in picker", "plugin", "vst", "synth", "insert", "fx" },
                    .perform = [this] { showPluginPicker(); },
                    .isEnabled = [this] { return projectContext.hasProject(); } });

    browser.addCommands (registry); // saving presets, which the browser shows

    // Showing and hiding apply to the main window; a panel in its own window is closed there.
    add (
        "view.toggleBrowser", "Show or Hide the Browser", "Shows or hides the browser on the left.", { "files", "samples", "plugins" },
        [this]
        {
            browserShown = ! browserShown;
            updateLayout();
        },
        [this] { return browserWindow == nullptr; });

    add (
        "view.toggleInspector", "Show or Hide the Inspector", "Shows or hides the inspector on the right.", { "properties" },
        [this]
        {
            inspectorShown = ! inspectorShown;
            updateLayout();
        },
        [this] { return inspectorWindow == nullptr; });

    add (
        "view.toggleEditor", "Show or Hide the Editor", "Shows or hides the editor dock under the playlist.", { "dock" },
        [this]
        {
            editorShown = ! editorShown;
            updateLayout();
        },
        [this] { return editorWindow == nullptr; });

    add ("view.pianoRoll", "Piano Roll", "Shows the piano roll, or hides it if it is showing.", { "notes", "midi" },
         [this] { showEditor (EditorDock::pianoRoll); });

    add ("view.mixer", "Mixer", "Shows the mixer, or hides it if it is showing.", { "channels", "faders" },
         [this] { showEditor (EditorDock::mixer); });

    add ("view.layoutBeat", "Beat Layout", "Arranges the panels for making beats: playlist, piano roll and browser.", { "layout" },
         [this] { setLayout (Layout::beat); });

    add ("view.layoutMix", "Mix Layout", "Arranges the panels for mixing: a large mixer and the inspector.", { "layout" },
         [this] { setLayout (Layout::mix); });

    add ("view.layoutFull", "Full Layout", "Shows every panel.", { "layout", "all" }, [this] { setLayout (Layout::full); });

    struct DetachCommand
    {
        DetachablePanel panel;
        const char* id;
        const char* name;
    };

    for (const auto& command : { DetachCommand { DetachablePanel::browser, "view.detachBrowser", "Browser in Its Own Window" },
                                 DetachCommand { DetachablePanel::inspector, "view.detachInspector", "Inspector in Its Own Window" },
                                 DetachCommand { DetachablePanel::editor, "view.detachEditor", "Editor in Its Own Window" } })
    {
        const auto which = command.panel;
        registry.add ({ .id = command.id,
                        .name = command.name,
                        .category = "View",
                        .description = "Moves the panel to a window of its own, for example on a second monitor, or back again.",
                        .synonyms = { "detach", "undock", "float", "second monitor", "window" },
                        .perform = [this, which] { setDetached (which, ! isDetached (which)); },
                        .isTicked = [this, which] { return isDetached (which); } });
    }

    // The editor used last, or the playlist while that one is hidden.
    addTimelineCommands (registry, [this]() -> TimelineEditor*
                         { return activeEditor != nullptr && activeEditor->isActive() ? activeEditor : &playlist; });
    playlist.addCommands (registry);

    // The note commands use the piano roll's grid.
    addNoteCommands (registry, projectContext, [this] { return pianoRoll.getGridStepInBeats(); });
    pianoRoll.addCommands (registry);
    guide.addCommands (registry);
    refreshShortcutTexts();
}

void AppShell::refreshShortcutTexts()
{
    topBar.refreshTooltips();
    playlist.refreshShortcutTexts();
    pianoRoll.refreshShortcutTexts();
}

juce::String AppShell::getDefaultHint() const
{
    if (auto next = guide.getHint(); next.isNotEmpty())
        return next;

    const auto paletteKey = getKeyText (commands, "view.commandPalette");
    return paletteKey.isEmpty() ? juce::String ("Search for any command with \"Search commands\" at the top right.")
                                : "Press " + paletteKey + " to search for any command.";
}
} // namespace phazite::ui
